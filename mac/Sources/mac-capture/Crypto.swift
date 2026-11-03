// Sideframe crypto: handshake tags, encrypted session (ChaCha20-Poly1305) and the pairing maths. See docs/PROTOCOL.md.
// Must stay byte-for-byte compatible with the Frame's src/mdcrypto.h (check: `mac-capture selftest` vs `sideframe --selftest`).
import Foundation
import CryptoKit
import CommonCrypto
import Security

extension Data {
    init?(hex: String) {
        guard hex.count % 2 == 0 else { return nil }
        var out = Data(capacity: hex.count / 2); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            guard let b = UInt8(hex[i..<j], radix: 16) else { return nil }
            out.append(b); i = j
        }
        self = out
    }
    var hex: String { map { String(format: "%02x", $0) }.joined() }
}

func randomBytes(_ n: Int) -> Data {
    var b = [UInt8](repeating: 0, count: n)
    _ = SecRandomCopyBytes(kSecRandomDefault, n, &b)
    return Data(b)
}

func constantTimeEqual(_ a: Data, _ b: Data) -> Bool {
    guard a.count == b.count else { return false }
    var diff: UInt8 = 0
    for (x, y) in zip(a, b) { diff |= x ^ y }
    return diff == 0
}

func sha256(_ d: Data) -> Data { Data(SHA256.hash(data: d)) }
func hmac(_ key: Data, _ msg: Data) -> Data { Data(HMAC<SHA256>.authenticationCode(for: msg, using: SymmetricKey(data: key))) }

/// Identifier of a pairing key: first 8 bytes of SHA-256(psk). Sent in HELLO so the Mac can pick the right key.
func keyId(for psk: Data) -> Data { sha256(psk).prefix(8) }

enum Handshake {
    static func tag(_ psk: Data, label: String, _ parts: Data...) -> Data {
        var m = Data(label.utf8); for p in parts { m.append(p) }
        return hmac(psk, m).prefix(16)
    }
    static func helloTag(_ psk: Data, kid: Data, cn: Data) -> Data { tag(psk, label: "sideframe-hello", kid, cn) }
    static func welcomeTag(_ psk: Data, cn: Data, sn: Data) -> Data { tag(psk, label: "sideframe-welcome", cn, sn) }
}

/// 2048-packet anti-replay window. accept() must only be called for packets that already passed authentication.
struct ReplayWindow {
    private var highest: UInt64 = 0, any = false
    private var seen = [Bool](repeating: false, count: 2048)
    mutating func accept(_ c: UInt64) -> Bool {
        if !any { any = true; highest = c; seen[Int(c % 2048)] = true; return true }
        if c > highest {
            let gap = c - highest
            if gap >= 2048 { for i in 0..<2048 { seen[i] = false } }
            else { for k in 1...Int(gap) { seen[Int((highest + UInt64(k)) % 2048)] = false } }
            highest = c; seen[Int(c % 2048)] = true; return true
        }
        if highest - c >= 2048 { return false }
        let i = Int(c % 2048)
        if seen[i] { return false }
        seen[i] = true; return true
    }
}

final class SecureSession {
    private let sendKey: SymmetricKey, recvKey: SymmetricKey
    private let sendPrefix: Data, recvPrefix: Data
    private var sendCounter: UInt64 = 0
    private var window = ReplayWindow()
    private let lock = NSLock()

    /// `server == true` for the Mac (sends Mac->Frame "m2f", receives "f2m").
    init(psk: Data, cn: Data, sn: Data, server: Bool) {
        func derive(_ info: String) -> (SymmetricKey, Data) {
            let okm = HKDF<SHA256>.deriveKey(inputKeyMaterial: SymmetricKey(data: psk), salt: cn + sn, info: Data(info.utf8), outputByteCount: 36)
            let bytes = okm.withUnsafeBytes { Data($0) }
            return (SymmetricKey(data: bytes.prefix(32)), Data(bytes.suffix(4)))
        }
        let m2f = derive("sideframe m2f"), f2m = derive("sideframe f2m")
        (sendKey, sendPrefix) = server ? m2f : f2m
        (recvKey, recvPrefix) = server ? f2m : m2f
    }

    private func nonce(_ prefix: Data, _ ctr: Data) -> ChaChaPoly.Nonce { try! ChaChaPoly.Nonce(data: prefix + ctr) }

    func seal(_ plain: Data, counterOverride: UInt64? = nil) -> Data {
        lock.lock(); let c = counterOverride ?? sendCounter; if counterOverride == nil { sendCounter += 1 }; lock.unlock()
        var be = c.bigEndian; let ctr = Data(bytes: &be, count: 8)
        let header = Data([0x03]) + ctr
        let box = try! ChaChaPoly.seal(plain, using: sendKey, nonce: nonce(sendPrefix, ctr), authenticating: header)
        return header + box.ciphertext + box.tag
    }

    /// Returns the plaintext, or nil if the packet is forged, corrupted, or a replay.
    func open(_ pkt: Data) -> Data? {
        guard pkt.count >= 9 + 16, pkt[pkt.startIndex] == 0x03 else { return nil }
        let b = pkt.startIndex
        let ctrData = Data(pkt[(b + 1)..<(b + 9)])
        let ctr = ctrData.reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
        let header = Data(pkt[b..<(b + 9)])
        let ct = Data(pkt[(b + 9)..<(pkt.endIndex - 16)]), tag = Data(pkt[(pkt.endIndex - 16)...])
        guard let box = try? ChaChaPoly.SealedBox(nonce: nonce(recvPrefix, ctrData), ciphertext: ct, tag: tag),
              let plain = try? ChaChaPoly.open(box, using: recvKey, authenticating: header) else { return nil }
        lock.lock(); let ok = window.accept(ctr); lock.unlock()
        return ok ? plain : nil
    }
}

/// The maths of pairing (docs/PROTOCOL.md section 2). Pure functions so the Frame's C implementation can be cross-checked.
enum PairCrypto {
    static let iterations = 600_000

    static func codeKey(code: String, salt: Data, nF: Data, nM: Data, iterations: Int = PairCrypto.iterations) -> Data {
        let pw = Array(code.utf8), s = Array(salt + nF + nM)
        var out = [UInt8](repeating: 0, count: 32)
        let rc = CCKeyDerivationPBKDF(CCPBKDFAlgorithm(kCCPBKDF2), pw.map { Int8(bitPattern: $0) }, pw.count, s, s.count,
                                      CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA256), UInt32(iterations), &out, out.count)
        precondition(rc == kCCSuccess, "PBKDF2 failed")
        return Data(out)
    }
    static func transcript(pkF: Data, pkM: Data, nF: Data, nM: Data, salt: Data) -> Data {
        sha256(Data("sideframe-pair-v1".utf8) + pkF + pkM + nF + nM + salt)
    }
    static func confirm(_ role: String, codeKey: Data, transcript: Data, dh: Data) -> Data {
        hmac(codeKey, Data(role.utf8) + transcript + dh)
    }
    static func psk(dh: Data, transcript: Data) -> Data {
        let k = HKDF<SHA256>.deriveKey(inputKeyMaterial: SymmetricKey(data: dh), salt: transcript, info: Data("sideframe psk v1".utf8), outputByteCount: 32)
        return k.withUnsafeBytes { Data($0) }
    }
    /// X25519 shared secret; nil if the peer's key is invalid / low order.
    static func dh(_ sk: Curve25519.KeyAgreement.PrivateKey, _ peerPub: Data) -> Data? {
        guard let pk = try? Curve25519.KeyAgreement.PublicKey(rawRepresentation: peerPub),
              let ss = try? sk.sharedSecretFromKeyAgreement(with: pk) else { return nil }
        let d = ss.withUnsafeBytes { Data($0) }
        return d.contains(where: { $0 != 0 }) ? d : nil
    }
}

/// Known-answer output for cross-checking against the C implementation on the Frame (fixed, public test values only).
func cryptoSelfTest() {
    // --- session handshake
    let psk = Data((0..<32).map { UInt8($0) })
    let kid = keyId(for: psk)
    let cn = Data((0..<16).map { UInt8(0x10 + $0) }), sn = Data((0..<16).map { UInt8(0x20 + $0) })
    print("kid          ", kid.hex)
    print("hello_tag    ", Handshake.helloTag(psk, kid: kid, cn: cn).hex)
    print("welcome_tag  ", Handshake.welcomeTag(psk, cn: cn, sn: sn).hex)
    let mac = SecureSession(psk: psk, cn: cn, sn: sn, server: true)
    let pkt = mac.seal(Data("known answer test".utf8), counterOverride: 7)
    print("m2f_packet   ", pkt.hex)
    let frame = SecureSession(psk: psk, cn: cn, sn: sn, server: false)
    print("frame_opens  ", frame.open(pkt).map { String(decoding: $0, as: UTF8.self) } ?? "FAILED")
    print("replay_rejected", frame.open(pkt) == nil)
    var tampered = pkt; tampered[tampered.startIndex + 12] ^= 1
    print("tamper_rejected", SecureSession(psk: psk, cn: cn, sn: sn, server: false).open(tampered) == nil)
    print("wrong_key_rejected", SecureSession(psk: Data(repeating: 9, count: 32), cn: cn, sn: sn, server: false).open(pkt) == nil)
    print("f2m_packet   ", frame.seal(Data("from frame".utf8), counterOverride: 3).hex)

    // --- pairing maths with fixed keys
    let skF = try! Curve25519.KeyAgreement.PrivateKey(rawRepresentation: Data((0..<32).map { UInt8(0x40 + $0) }))
    let skM = try! Curve25519.KeyAgreement.PrivateKey(rawRepresentation: Data((0..<32).map { UInt8(0x80 + $0) }))
    let pkF = skF.publicKey.rawRepresentation, pkM = skM.publicKey.rawRepresentation
    let nF = Data((0..<16).map { UInt8(0x50 + $0) }), nM = Data((0..<16).map { UInt8(0x60 + $0) }), salt = Data((0..<16).map { UInt8(0x70 + $0) })
    let t0 = Date()
    let ck = PairCrypto.codeKey(code: "12345678", salt: salt, nF: nF, nM: nM)
    let kdfMs = Date().timeIntervalSince(t0) * 1000
    let dhF = PairCrypto.dh(skF, pkM)!, dhM = PairCrypto.dh(skM, pkF)!
    let T = PairCrypto.transcript(pkF: pkF, pkM: pkM, nF: nF, nM: nM, salt: salt)
    print("pair_pkF     ", pkF.hex)
    print("pair_pkM     ", pkM.hex)
    print("pair_dh      ", dhF.hex, dhF == dhM ? "(both sides equal)" : "MISMATCH")
    print("pair_codeKey ", ck.hex, String(format: "(PBKDF2 %d iterations took %.0f ms)", PairCrypto.iterations, kdfMs))
    print("pair_T       ", T.hex)
    print("pair_confF   ", PairCrypto.confirm("F", codeKey: ck, transcript: T, dh: dhF).hex)
    print("pair_confM   ", PairCrypto.confirm("M", codeKey: ck, transcript: T, dh: dhF).hex)
    let pairPsk = PairCrypto.psk(dh: dhF, transcript: T)
    print("pair_psk     ", pairPsk.hex)
    print("pair_kid     ", keyId(for: pairPsk).hex)
}
