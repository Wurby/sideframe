// mac-capture — command-line companion of the Sideframe Mac app (development / headless use / tests).
//   mac-capture serve  [--port 5000] [--mbps N (default: automatic from the display size)] [--fps 60] [--seconds N] [--no-capture]   wait for a paired Frame, then stream
//   mac-capture pair   [--port 5000]                                         start pairing: prints a code, waits for the Frame
//   mac-capture list                                                         list paired Frames
//   mac-capture record <out.h265> [seconds] [fps] [mbps]                     capture to a file (no network)
//   mac-capture selftest                                                     crypto known-answer output (cross-checked against the Frame)
import Foundation
import CoreGraphics
import VideoToolbox

setvbuf(stdout, nil, _IOLBF, 0)   // line-buffered so logs are visible live

func usage() -> Never {
    print("usage: mac-capture serve [--port N] [--mbps N] [--fps N] [--seconds N] | pair [--port N] | list | record <out.h265> [seconds] [fps] [mbps] | selftest")
    exit(2)
}
func opt(_ name: String, _ def: Int) -> Int {
    let a = CommandLine.arguments
    if let i = a.firstIndex(of: name), i + 1 < a.count, let v = Int(a[i + 1]) { return v }
    return def
}
func requireScreenAccess() {
    if !CGPreflightScreenCaptureAccess() {
        print("NO Screen Recording permission for this process — run it from Terminal (not over SSH) and allow it in System Settings > Privacy & Security.")
        CGRequestScreenCaptureAccess()
    }
}

let command = CommandLine.arguments.dropFirst().first ?? ""
switch command {
case "selftest":
    cryptoSelfTest(); exit(0)

case "list":
    let all = KeyStore.shared.all
    print("This Mac: \(KeyStore.shared.macName)  id \(KeyStore.shared.macId.hex)")
    if all.isEmpty { print("No paired Frames.") }
    for f in all { print("  \(f.name)  key id \(f.kid)  paired \(f.added)") }
    exit(0)

case "pair":
    let port = UInt16(opt("--port", 5000))
    PairingManager.shared.onEvent = { ev in
        switch ev {
        case .started(let code, _): print("Pairing code: \(PairingManager.display(code))   (valid 2 minutes; enter it on your Steam Frame)")
        case .frameConnecting(let n): print("\(n) is pairing…")
        case .wrongCode(let left): print("Wrong code entered. \(left) attempt(s) left.")
        case .succeeded(let n): print("Paired with \(n)."); exit(0)
        case .failed(let why): print("Pairing failed: \(why)"); exit(1)
        case .ended: exit(1)
        }
    }
    do { try Server.shared.start(port: port) } catch { print("ERROR: \(error)"); exit(1) }
    PairingManager.shared.begin()
    RunLoop.main.run()

case "record":
    let a = CommandLine.arguments
    guard a.count >= 3 else { usage() }
    let secs = a.count > 3 ? Double(a[3]) ?? 5 : 5, fps = a.count > 4 ? Int(a[4]) ?? 60 : 60, mbps = a.count > 5 ? Int(a[5]) ?? 20 : 20
    requireScreenAccess()
    FileManager.default.createFile(atPath: a[2], contents: nil)
    outFile = try FileHandle(forWritingTo: URL(fileURLWithPath: a[2]))
    Task {
        do {
            let (stream, cap) = try await startCapture(fps: fps, mbps: mbps)
            try await Task.sleep(nanoseconds: UInt64(secs * 1e9))
            try await stream.stopCapture()
            VTCompressionSessionCompleteFrames(cap.enc.session, untilPresentationTimeStamp: .invalid)
            try? outFile?.close()
            print("captured \(cap.gotFrames) frames, encoded \(cap.enc.frames) (\(cap.enc.keyframes) key), \(cap.enc.bytes / 1024) KB -> \(a[2])")
            exit(0)
        } catch { print("ERROR: \(error)"); exit(1) }
    }
    RunLoop.main.run()

case "serve":
    let port = UInt16(opt("--port", 5000)), mbps = opt("--mbps", 0), fps = opt("--fps", 60), secs = opt("--seconds", 0)
    if !CommandLine.arguments.contains("--no-capture") { requireScreenAccess() }
    if KeyStore.shared.all.isEmpty { print("No Frame is paired yet. Run `mac-capture pair` (or use the Sideframe app) and enter the code on your Steam Frame.") }
    Server.shared.onSessionStart = { resetSessionState() }
    Server.shared.onSessionEnd = { report(); InputInjector.shared.releaseAll() }
    do { try Server.shared.start(port: port) } catch { print("ERROR: \(error)"); exit(1) }
    print("Waiting for a paired Frame on UDP port \(port) (\(KeyStore.shared.all.count) paired; others are ignored)")
    if CommandLine.arguments.contains("--no-capture") {   // network/security testing without screen capture (e.g. over SSH)
        if secs > 0 { DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(secs)) { print("done. handshakes: \(Server.shared.handshakes), rejected packets: \(Server.shared.rejected)"); exit(0) } }
        RunLoop.main.run()
    }
    Task {
        do {
            let (stream, _) = try await startCapture(fps: fps, mbps: mbps)
            if secs > 0 { try await Task.sleep(nanoseconds: UInt64(secs) * 1_000_000_000) } else { while true { try await Task.sleep(nanoseconds: 1_000_000_000) } }
            try await stream.stopCapture()
            if Server.shared.hasSession { report() }
            print("done. handshakes: \(Server.shared.handshakes), rejected packets (unknown key / forged / replayed): \(Server.shared.rejected)")
            exit(0)
        } catch { print("ERROR: \(error)"); exit(1) }
    }
    RunLoop.main.run()

default:
    usage()
}
