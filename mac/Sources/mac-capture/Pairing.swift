// Mac side of pairing (docs/PROTOCOL.md section 2): the user opens "Pair a Steam Frame...", we show a single-use 8-digit code,
// the Frame sends its key + the typed code's proof, and on success both sides store a fresh 256-bit pairing key.
import Foundation
import CryptoKit
import Darwin

enum PairEvent {
    case started(code: String, expires: Date)
    case frameConnecting(String)
    case wrongCode(attemptsLeft: Int)
    case succeeded(String)
    case failed(String)      // pairing mode ended without success (expired, too many wrong codes)
    case ended               // cancelled by the user
}

final class PairingManager {
    static let shared = PairingManager()
    var onEvent: ((PairEvent) -> Void)?        // may be called from any thread

    private struct Pending { var from: sockaddr_in; var skM: Curve25519.KeyAgreement.PrivateKey; var pkF, nF, nM, salt: Data; var name: String; var at: Date }
    private let lock = NSLock()
    private var code: String?
    private var expires = Date.distantPast
    private var attempts = 0
    private var pending: Pending?
    private var timer: DispatchSourceTimer?
    // The code does not expire on a timer: it stays until the pairing window is closed, a new code is requested, pairing succeeds,
    // or three wrong guesses are made (then it is dead until the user asks for a new one).
    private let maxAttempts = 3

    var isActive: Bool { lock.lock(); defer { lock.unlock() }; return code != nil && Date() < expires }

    static func makeCode() -> String {
        var v: UInt32 = 0
        repeat { v = randomBytes(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) } } while v >= 4_200_000_000   // no modulo bias
        return String(format: "%08d", v % 100_000_000)
    }
    /// "12345678" -> "1234 5678"
    static func display(_ code: String) -> String { code.count == 8 ? "\(code.prefix(4)) \(code.suffix(4))" : code }

    @discardableResult func begin() -> (code: String, expires: Date) {
        lock.lock()
        let c = PairingManager.makeCode()
        code = c; expires = .distantFuture; attempts = 0; pending = nil
        let e = expires
        timer?.cancel(); timer = nil
        lock.unlock()
        onEvent?(.started(code: c, expires: e))
        return (c, e)
    }
    func cancel() {
        lock.lock(); let wasActive = code != nil; code = nil; pending = nil; timer?.cancel(); timer = nil; lock.unlock()
        if wasActive { onEvent?(.ended) }
    }
    private func expireIfCurrent(_ c: String) {
        lock.lock(); let same = (code == c); if same { code = nil; pending = nil; timer?.cancel(); timer = nil }; lock.unlock()
        if same { onEvent?(.failed("The code expired.")) }
    }

    private static func clean(_ raw: Data) -> String {
        let s = String(decoding: raw, as: UTF8.self).unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) }
        let t = String(String.UnicodeScalarView(s)).trimmingCharacters(in: .whitespaces)
        return t.isEmpty ? "Steam Frame" : String(t.prefix(48))
    }

    /// Returns true if the datagram was a pairing message (so the caller doesn't treat it as anything else).
    func handle(_ pkt: Data, from: sockaddr_in, reply: (Data) -> Void) -> Bool {
        guard let type = pkt.first else { return false }
        switch type {
        case 0x10: request(pkt, from: from, reply: reply); return true
        case 0x12: confirm(pkt, from: from, reply: reply); return true
        default: return false
        }
    }

    private func denied(_ reason: UInt8, _ reply: (Data) -> Void) { reply(Data([0x1F, reason])) }

    private func request(_ pkt: Data, from: sockaddr_in, reply: (Data) -> Void) {
        let b = pkt.startIndex
        guard pkt.count >= 51, pkt[b + 1] == 1 else { return }
        let nameLen = Int(pkt[b + 50])
        guard pkt.count >= 51 + nameLen else { return }
        lock.lock()
        let active = code != nil && Date() < expires
        let busy = pending.map { !sockaddrEqual($0.from, from) && Date().timeIntervalSince($0.at) < 10 } ?? false
        lock.unlock()
        guard active else { denied(1, reply); return }
        guard !busy else { denied(3, reply); return }

        let skM = Curve25519.KeyAgreement.PrivateKey()
        let nM = randomBytes(16), salt = randomBytes(16)
        let p = Pending(from: from, skM: skM, pkF: Data(pkt[(b + 2)..<(b + 34)]), nF: Data(pkt[(b + 34)..<(b + 50)]), nM: nM, salt: salt,
                        name: PairingManager.clean(Data(pkt[(b + 51)..<(b + 51 + nameLen)])), at: Date())
        lock.lock(); pending = p; lock.unlock()
        onEvent?(.frameConnecting(p.name))
        let macName = Data(KeyStore.shared.macName.utf8.prefix(60))
        reply(Data([0x11]) + skM.publicKey.rawRepresentation + nM + salt + KeyStore.shared.macId + Data([UInt8(macName.count)]) + macName)
    }

    private func confirm(_ pkt: Data, from: sockaddr_in, reply: (Data) -> Void) {
        guard pkt.count == 33 else { return }
        let confF = Data(pkt.suffix(32))
        lock.lock()
        guard let p = pending, sockaddrEqual(p.from, from), let c = code, Date() < expires else { lock.unlock(); return }
        pending = nil                      // one confirmation per request: a guess always costs a fresh handshake
        lock.unlock()

        let ck = PairCrypto.codeKey(code: c, salt: p.salt, nF: p.nF, nM: p.nM)
        guard let dh = PairCrypto.dh(p.skM, p.pkF) else { denied(2, reply); return }
        let T = PairCrypto.transcript(pkF: p.pkF, pkM: p.skM.publicKey.rawRepresentation, nF: p.nF, nM: p.nM, salt: p.salt)
        guard constantTimeEqual(PairCrypto.confirm("F", codeKey: ck, transcript: T, dh: dh), confF) else {
            lock.lock(); attempts += 1; let left = maxAttempts - attempts
            if left <= 0 { code = nil; timer?.cancel(); timer = nil }
            lock.unlock()
            denied(left <= 0 ? 4 : 2, reply)
            onEvent?(left <= 0 ? .failed("Too many wrong codes. Start pairing again for a new code.") : .wrongCode(attemptsLeft: left))
            return
        }
        let psk = PairCrypto.psk(dh: dh, transcript: T)
        KeyStore.shared.add(name: p.name, key: psk)
        lock.lock(); code = nil; timer?.cancel(); timer = nil; lock.unlock()
        reply(Data([0x13]) + PairCrypto.confirm("M", codeKey: ck, transcript: T, dh: dh) + KeyStore.shared.macId)
        onEvent?(.succeeded(p.name))
    }
}
