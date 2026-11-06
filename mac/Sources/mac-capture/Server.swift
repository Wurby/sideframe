// Authenticated UDP server. A Frame connects (HELLO with the id of its pairing key); only a peer that proves knowledge of a paired
// key gets a session. Video goes out as encrypted fragments; the Frame sends encrypted acks / NACKs / keyframe requests / pings / input.
// Unpaired devices can only talk to the pairing engine, and only while the user has "Pair a Steam Frame..." open.
import Foundation
import Darwin

let UPAY = 1350   // payload bytes per fragment (+16 inner header, +25 crypto framing: stays well under a 1500-byte MTU)

func sockaddrEqual(_ a: sockaddr_in, _ b: sockaddr_in) -> Bool { a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port }
func describe(_ a: sockaddr_in) -> String {
    var addr = a.sin_addr; var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
    inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN))
    return "\(String(cString: buf)):\(UInt16(bigEndian: a.sin_port))"
}

final class Server {
    static let shared = Server()
    private(set) var serving = false
    private var fd: Int32 = -1
    private let lock = NSLock()
    private var active: SecureSession?
    private var activeName = ""
    private var peer = sockaddr_in()
    private var lastRxNs: UInt64 = 0
    private var pendingSession: SecureSession?
    private var pendingName = ""
    private var pendingPeer = sockaddr_in()
    private var pendingAtNs: UInt64 = 0
    private let histLock = NSLock()
    private var history: [UInt32: [Data]] = [:]
    private var histOrder: [UInt32] = []
    private(set) var rejected = 0, handshakes = 0
    var onSessionStart: (() -> Void)?
    var onSessionEnd: (() -> Void)?
    /// The Frame told us its tab is hidden (true) or visible again (false): the app stops/starts screen capture accordingly.
    private(set) var paused = false
    var onPauseChanged: (() -> Void)?
    /// Input messages (types 5...10) from the authenticated Frame. Set by the app / CLI.
    var onInput: ((UInt8, [UInt8]) -> Void)? = { type, m in InputInjector.shared.handle(type: type, msg: m) }

    var hasSession: Bool { lock.lock(); defer { lock.unlock() }; return active != nil }
    var peerDescription: String { lock.lock(); defer { lock.unlock() }; return describe(peer) }
    var activeFrameName: String { lock.lock(); defer { lock.unlock() }; return activeName }

    func start(port: UInt16) throws {
        fd = socket(AF_INET, SOCK_DGRAM, 0)
        var one: Int32 = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var buf: Int32 = 2 << 20
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, socklen_t(MemoryLayout<Int32>.size))
        var a = sockaddr_in(); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
        a.sin_port = port.bigEndian; a.sin_addr.s_addr = INADDR_ANY
        let rc = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard rc == 0 else { throw NSError(domain: "bind", code: Int(errno), userInfo: [NSLocalizedDescriptionKey: "bind to UDP \(port) failed: \(String(cString: strerror(errno)))"]) }
        serving = true
        Thread.detachNewThread { [self] in rxLoop() }
        Thread.detachNewThread { [self] in watchdog() }
    }

    // ---- raw I/O ----
    func sendRaw(_ d: Data, to: sockaddr_in) {
        var dest = to
        d.withUnsafeBytes { raw in
            withUnsafePointer(to: &dest) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                _ = sendto(fd, raw.baseAddress, d.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        }
    }
    /// Encrypt + send one plaintext message to the active Frame (no-op without a session).
    func sendPlain(_ plain: Data) {
        lock.lock(); let s = active; let to = peer; lock.unlock()
        guard let s else { return }
        sendRaw(s.seal(plain), to: to)
    }

    // ---- receive path ----
    private func rxLoop() {
        var buf = [UInt8](repeating: 0, count: 2048)
        while true {
            var sa = sockaddr_in(); var sl = socklen_t(MemoryLayout<sockaddr_in>.size)
            let r = withUnsafeMutablePointer(to: &sa) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &sl) } }
            if r <= 0 { if errno != EINTR { usleep(10_000) }; continue }
            handle(Data(buf[0..<r]), from: sa)
        }
    }

    private let debug = ProcessInfo.processInfo.environment["SIDEFRAME_DEBUG"] == "1"
    private func handle(_ pkt: Data, from sa: sockaddr_in) {
        let b = pkt.startIndex
        if debug { print("rx \(pkt.count) B type 0x\(String(pkt[b], radix: 16)) from \(describe(sa))") }
        switch pkt[b] {
        case 0x01 where pkt.count == 41:   // HELLO: kid(8) cn(16) tag(16)
            let kid = Data(pkt[(b + 1)..<(b + 9)]), cn = Data(pkt[(b + 9)..<(b + 25)]), tag = Data(pkt[(b + 25)..<(b + 41)])
            guard let psk = KeyStore.shared.key(forKid: kid.hex),
                  constantTimeEqual(tag, Handshake.helloTag(psk, kid: kid, cn: cn)) else { rejected += 1; if debug { print("  HELLO rejected (unknown key id or bad tag)") }; return }
            let sn = randomBytes(16)
            lock.lock(); pendingSession = SecureSession(psk: psk, cn: cn, sn: sn, server: true); pendingName = KeyStore.shared.name(forKid: kid.hex) ?? "Steam Frame"
            pendingPeer = sa; pendingAtNs = nowNs(); lock.unlock()
            handshakes += 1
            sendRaw(Data([0x02]) + sn + Handshake.welcomeTag(psk, cn: cn, sn: sn), to: sa)
        case 0x03:
            handleData(pkt, from: sa)
        case 0x10, 0x12:                   // pairing messages
            if !PairingManager.shared.handle(pkt, from: sa, reply: { self.sendRaw($0, to: sa) }) { rejected += 1 }
        default:
            rejected += 1
        }
    }

    private func handleData(_ pkt: Data, from sa: sockaddr_in) {
        lock.lock(); let act = active, ap = peer, pend = pendingSession, pp = pendingPeer, pat = pendingAtNs, pname = pendingName; lock.unlock()
        if let act, sockaddrEqual(ap, sa), let plain = act.open(pkt) {
            lock.lock(); lastRxNs = nowNs(); lock.unlock()
            dispatch(plain); return
        }
        // First authenticated packet under the pending keys proves the peer completed the handshake -> promote.
        // (A replayed HELLO from an attacker can create a pending session, but never gets promoted: it cannot produce valid DATA.)
        if let pend, sockaddrEqual(pp, sa), nowNs() - pat < 10_000_000_000, let plain = pend.open(pkt) {
            paused = false
            lock.lock(); let replaced = active != nil; active = pend; activeName = pname; peer = sa; lastRxNs = nowNs(); pendingSession = nil; lock.unlock()
            if replaced { onSessionEnd?() }
            print("\(pname) authenticated from \(describe(sa)) — session started")
            onSessionStart?()
            dispatch(plain); return
        }
        rejected += 1
    }

    /// Plaintext message from the Frame: [u8 type][u8 0][u16 n][u32 seq][u32 val] (+ payload)
    private func dispatch(_ plain: Data) {
        guard plain.count >= 12 else { return }
        let m = [UInt8](plain)
        let type = m[0], n = Int(m[2]) << 8 | Int(m[3])
        let seq = UInt32(m[4]) << 24 | UInt32(m[5]) << 16 | UInt32(m[6]) << 8 | UInt32(m[7])
        let val = UInt32(m[8]) << 24 | UInt32(m[9]) << 16 | UInt32(m[10]) << 8 | UInt32(m[11])
        switch type {
        case 1:   // cumulative ack
            let tAck = nowNs()
            statsLock.lock()
            if seq &+ 1 > ackedNext || ackedNext &- seq > 1_000_000 { ackedNext = seq &+ 1 }
            outstanding = max(0, Int(nextSeq) - Int(ackedNext))
            lastAckNs = tAck
            if let p = pending.removeValue(forKey: seq), val != 0xFFFF_FFFF {
                let frameUs = Double(val), rtt = Double(tAck - p.tSent) / 1000 - frameUs
                done.append(Sample(capLagUs: p.capLag, encUs: p.enc, sendUs: p.send, netRtt: rtt, frameUs: frameUs))
                if rtt < minRttUs { minRttUs = rtt }
            }
            if pending.count > 600 { pending = pending.filter { $0.key >= ackedNext } }
            statsLock.unlock()
        case 2:   // NACK: resend requested fragments from history
            histLock.lock(); let pk = history[seq]; histLock.unlock()
            guard let pk else { return }
            var list: [Int] = []
            if n == 0xFFFF { list = Array(0..<pk.count) }
            else { for i in 0..<n where 12 + 2 * i + 1 < m.count { list.append(Int(m[12 + 2 * i]) << 8 | Int(m[13 + 2 * i])) } }
            for idx in list where idx < pk.count { var q = pk[idx]; q[q.startIndex + 8] |= 2; sendPlain(q); statsLock.lock(); retxSent += 1; statsLock.unlock() }
            statsLock.lock(); nacksRx += 1; statsLock.unlock()
        case 12:  // QUALITY: seq = frame-rate ceiling (0 = none), val = percent of the automatic bitrate (clamped 25...100)
            statsLock.lock(); wantFps = Int(min(seq, 120)); wantPct = Int(min(max(val, 25), 100)); statsLock.unlock()
        case 3:   // keyframe request
            statsLock.lock(); forceKey = true; keyRequests += 1; statsLock.unlock()
        case 13:  // BYE: the Frame is leaving: end the session now instead of waiting for the 4 s silence timeout
            lock.lock(); let had = active != nil; active = nil; lock.unlock()
            if had { print("Frame said goodbye — session ended"); onSessionEnd?() }
        case 11:  // PAUSE (a != 0) / RESUME (a == 0): the Frame's tab was hidden / shown
            let p = seq != 0
            if p != paused { paused = p; if !p { resetFlowControl() }; onPauseChanged?() }
        case 4:   // ping: echo the 12 bytes so the Frame can measure RTT
            sendPlain(Data(m[0..<12]))
        default:
            onInput?(type, m)
        }
    }

    private func watchdog() {
        while true {
            usleep(500_000)
            lock.lock(); let idle = active != nil && nowNs() - lastRxNs > 4_000_000_000
            if idle { active = nil }
            lock.unlock()
            if idle { print("Frame went quiet — session ended"); onSessionEnd?() }
        }
    }

    // ---- video out ----
    /// Inner packet: [u32 seq][u16 idx][u16 cnt][u8 flags 1=key 2=retx][u8 0][u16 len][u32 total] + payload (<= UPAY)
    func sendFrame(seq: UInt32, key: Bool, data: Data) {
        let cnt = (data.count + UPAY - 1) / UPAY
        var pkts: [Data] = []; pkts.reserveCapacity(cnt)
        for i in 0..<cnt {
            let lo = i * UPAY, hi = min(data.count, lo + UPAY)
            var h = Data(capacity: 16 + hi - lo)
            func be32(_ v: UInt32) { var b = v.bigEndian; h.append(Data(bytes: &b, count: 4)) }
            func be16(_ v: UInt16) { var b = v.bigEndian; h.append(Data(bytes: &b, count: 2)) }
            be32(seq); be16(UInt16(i)); be16(UInt16(cnt)); h.append(key ? 1 : 0); h.append(0); be16(UInt16(hi - lo)); be32(UInt32(data.count))
            h.append(data.subdata(in: (data.startIndex + lo)..<(data.startIndex + hi)))
            pkts.append(h)
        }
        histLock.lock()
        history[seq] = pkts; histOrder.append(seq)
        while histOrder.count > 120 { history.removeValue(forKey: histOrder.removeFirst()) }
        histLock.unlock()
        for (i, p) in pkts.enumerated() { sendPlain(p); if i % 12 == 11 { usleep(250) } }   // light pacing: don't dump a keyframe on the AP at once
    }
}
