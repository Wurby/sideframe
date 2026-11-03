import Foundation
import Darwin

// Tuning switches (environment). Defaults are the measured best settings.
let env = ProcessInfo.processInfo.environment
let useLL = env["LL"] != "0"                 // low-latency rate control (encode p50 32 -> 13 ms)
let keySec = Double(env["KEY"] ?? "") ?? 2   // keyframe interval, seconds
let useLimit = env["LIMIT"] != "0"           // cap bursts at 1.5x the average bitrate per second
let maxInflight = Int(env["WINDOW"] ?? "") ?? 6   // flow-control window (frames)
var tbInfo: mach_timebase_info_data_t = { var t = mach_timebase_info_data_t(); mach_timebase_info(&t); return t }()
func machToUs(_ ticks: UInt64) -> Double { Double(ticks) * Double(tbInfo.numer) / Double(tbInfo.denom) / 1000 }
func nowNs() -> UInt64 { DispatchTime.now().uptimeNanoseconds }
