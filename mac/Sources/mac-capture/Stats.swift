// Flow-control state and latency bookkeeping shared by the capture/encode path and the network thread.
import Foundation

var nextSeq: UInt32 = 0
let statsLock = NSLock()
var outstanding = 0, skippedByWindow = 0, keyRequests = 0
var forceKey = false
// What the Frame asked for (message type 12): a frame-rate ceiling and a share of the automatic bitrate. 0 / 100 = no request.
var wantFps = 0, wantPct = 100
var lastForcedKeyNs: UInt64 = 0
var ackedNext: UInt32 = 0
var lastAckNs: UInt64 = 0
var retxSent = 0, nacksRx = 0
struct Sample { var capLagUs = 0.0, encUs = 0.0, sendUs = 0.0, netRtt = 0.0, frameUs = 0.0 }
var pending: [UInt32: (tSent: UInt64, capLag: Double, enc: Double, send: Double)] = [:]
var done: [Sample] = []
var minRttUs = Double.infinity

/// Called when a new authenticated Frame session starts: fresh flow-control state, force a keyframe.
func resetSessionState() {
    statsLock.lock()
    outstanding = 0; ackedNext = nextSeq; lastAckNs = nowNs()
    pending.removeAll(); done.removeAll(); minRttUs = .infinity
    skippedByWindow = 0; keyRequests = 0; retxSent = 0; nacksRx = 0
    forceKey = true; lastForcedKeyNs = 0
    statsLock.unlock()
}

/// After a pause: forget frames that were in flight (they will never be acknowledged) and start with a keyframe.
func resetFlowControl() {
    statsLock.lock(); outstanding = 0; ackedNext = nextSeq; lastAckNs = nowNs(); forceKey = true; lastForcedKeyNs = 0; statsLock.unlock()
}

func pct(_ a: [Double], _ q: Double) -> Double { let s = a.sorted(); return s.isEmpty ? 0 : s[min(s.count - 1, Int(Double(s.count) * q))] }

func report() {
    statsLock.lock(); let d = done; let sk = skippedByWindow, kr = keyRequests, rs = retxSent, nk = nacksRx; statsLock.unlock()
    print("flow control: window=\(maxInflight) frames, skipped before encode: \(sk), keyframe requests from Frame: \(kr)")
    print("UDP: NACKs received: \(nk), fragments retransmitted: \(rs)")
    guard !d.isEmpty else { print("no latency samples"); return }
    func line(_ name: String, _ v: [Double]) {
        print(String(format: "  %-34@ p50 %7.1f  p95 %7.1f  p99 %7.1f  max %7.1f ms", name as NSString, pct(v, 0.5)/1000, pct(v, 0.95)/1000, pct(v, 0.99)/1000, (v.max() ?? 0)/1000))
    }
    print("LATENCY over \(d.count) frames (ms):")
    line("B encode (HEVC, hardware)", d.map { $0.encUs })
    line("C send (into socket)", d.map { $0.sendUs })
    line("D network round trip (RTT)", d.map { $0.netRtt })
    line("E Frame recv->present (incl decode)", d.map { $0.frameUs })
    line("ESTIMATED one-way (B+C+D/2+E)", d.map { $0.encUs + $0.sendUs + $0.netRtt / 2 + $0.frameUs })   // excludes SCK handoff + display scanout
    print(String(format: "  base network RTT (min seen): %.1f ms", minRttUs/1000))
}
