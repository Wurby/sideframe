// ScreenCaptureKit mirror of the main display -> VideoToolbox HEVC (hardware, low-latency) -> Annex B access units.
import Foundation
import ScreenCaptureKit
import VideoToolbox
import CoreMedia
import CoreGraphics

var outFile: FileHandle? = nil          // "record" mode only
let startCode: [UInt8] = [0, 0, 0, 1]

final class Encoder {
    var session: VTCompressionSession!
    var frames = 0, keyframes = 0, bytes = 0
    let q = DispatchQueue(label: "enc.out")
    init(width: Int, height: Int, fps: Int, bitrate: Int) throws {
        var spec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true]
        if useLL { spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
        var s: VTCompressionSession?
        let st = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height),
            codecType: kCMVideoCodecType_HEVC, encoderSpecification: spec as CFDictionary,
            imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil,
            compressionSessionOut: &s)
        guard st == noErr, let s else { throw NSError(domain: "vt", code: Int(st)) }
        session = s
        func set(_ k: CFString, _ v: Any) { VTSessionSetProperty(s, key: k, value: v as CFTypeRef) }
        set(kVTCompressionPropertyKey_RealTime, true)
        set(kVTCompressionPropertyKey_AllowFrameReordering, false)   // no B-frames: low latency
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, keySec)
        if useLimit { set(kVTCompressionPropertyKey_DataRateLimits, [Int(Double(bitrate) * 1.5 / 8), 1] as CFArray) }
        VTCompressionSessionPrepareToEncodeFrames(s)
    }
    /// Change the target bitrate while running (the Frame asked for less/more, e.g. because it is getting hot).
    func setBitrate(_ bitrate: Int) {
        guard let s = session else { return }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: bitrate as CFTypeRef)
        if useLimit { VTSessionSetProperty(s, key: kVTCompressionPropertyKey_DataRateLimits, value: [Int(Double(bitrate) * 1.5 / 8), 1] as CFArray) }
    }
    func encode(_ pb: CVPixelBuffer, pts: CMTime, tCb: UInt64, capLagUs: Double) {
        let seq = nextSeq; nextSeq &+= 1
        statsLock.lock(); outstanding += 1
        // honor keyframe requests at most every 400 ms (a lossy link otherwise floods us with requests -> keyframe storm)
        let tn = nowNs(); let fk = forceKey && (tn - lastForcedKeyNs > 400_000_000)
        if fk { forceKey = false; lastForcedKeyNs = tn }
        statsLock.unlock()
        let props: CFDictionary? = fk ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(session, imageBuffer: pb, presentationTimeStamp: pts, duration: .invalid,
                                        frameProperties: props, infoFlagsOut: nil) { [self] status, _, sbuf in
            guard status == noErr, let sbuf else { statsLock.lock(); outstanding = max(0, outstanding - 1); statsLock.unlock(); return }
            let tEnc = nowNs()
            q.sync { write(sbuf, seq: seq, tCb: tCb, tEnc: tEnc, capLagUs: capLagUs) }
        }
    }
    func write(_ sbuf: CMSampleBuffer, seq: UInt32, tCb: UInt64, tEnc: UInt64, capLagUs: Double) {
        var isKey = true
        if let att = CMSampleBufferGetSampleAttachmentsArray(sbuf, createIfNecessary: false) as? [[CFString: Any]],
           let a = att.first, (a[kCMSampleAttachmentKey_NotSync] as? Bool) == true { isKey = false }
        var data = Data()
        if isKey, let fd = CMSampleBufferGetFormatDescription(sbuf) {   // keyframes carry VPS/SPS/PPS in-band
            var count = 0
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil,
                parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            for i in 0..<count {
                var p: UnsafePointer<UInt8>?; var n = 0
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p,
                    parameterSetSizeOut: &n, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                if let p { data.append(contentsOf: startCode); data.append(p, count: n) }
            }
            keyframes += 1
        }
        guard let bb = CMSampleBufferGetDataBuffer(sbuf) else { return }
        var len = 0; var ptr: UnsafeMutablePointer<Int8>?
        CMBlockBufferGetDataPointer(bb, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr)
        guard let ptr else { return }
        let raw = UnsafeRawPointer(ptr); var off = 0       // AVCC (4-byte lengths) -> Annex B
        while off + 4 <= len {
            let n = Int(UInt32(bigEndian: raw.loadUnaligned(fromByteOffset: off, as: UInt32.self)))
            off += 4
            if n <= 0 || off + n > len { break }
            data.append(contentsOf: startCode); data.append(Data(bytes: raw + off, count: n)); off += n
        }
        outFile?.write(data)
        if Server.shared.serving && Server.shared.hasSession {
            let encUs = Double(tEnc - tCb) / 1000
            statsLock.lock(); pending[seq] = (nowNs(), capLagUs, encUs, 0); statsLock.unlock()
            Server.shared.sendFrame(seq: seq, key: isKey, data: data)
            let tSent = nowNs()
            statsLock.lock(); pending[seq] = (tSent, capLagUs, encUs, Double(tSent - tEnc) / 1000); statsLock.unlock()
        }
        frames += 1; bytes += data.count
    }
}

final class Capture: NSObject, SCStreamOutput {
    /// Stop the keepalive thread and release the hardware encoder (call after the SCStream has been stopped).
    func shutdown() {
        keepaliveOn = false
        lastLock.lock(); lastPB = nil; lastLock.unlock()
        if let s = enc?.session { VTCompressionSessionInvalidate(s) }
    }

    var enc: Encoder!
    var gotFrames = 0
    // ScreenCaptureKit only delivers a frame when the screen CHANGES. A new viewer (or a keyframe request) needs a
    // picture anyway, so the last captured frame is kept and re-encoded on demand.
    private let lastLock = NSLock()
    private var keepaliveOn = true
    private var sharpenedStamp: UInt64 = 0, sharpenCount = 0
    // quality requested by the Frame (Stats: wantFps / wantPct) applied to the running capture
    var stream: SCStream?; var cfg: SCStreamConfiguration?
    var baseFps = 60, baseBitrate = 20_000_000
    private var appliedFps = 0, appliedPct = 100
    func applyWantedQuality() {
        statsLock.lock(); let wf = wantFps, wp = wantPct; statsLock.unlock()
        let fps = wf > 0 ? min(wf, baseFps) : baseFps
        if fps != appliedFps, let cfg, let stream {
            appliedFps = fps; cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
            stream.updateConfiguration(cfg) { _ in }
            print("capture rate set to \(fps) fps (the Frame asked for \(wf == 0 ? "no limit" : "\(wf)"))")
        }
        if wp != appliedPct { appliedPct = wp; enc.setBitrate(baseBitrate * wp / 100); print("bitrate set to \(baseBitrate * wp / 100 / 1_000_000) Mbps (\(wp)%)") }
    }
    private var lastPB: CVPixelBuffer?
    private var lastEncodeNs: UInt64 = 0
    func startKeepalive() {
        Thread.detachNewThread { [self] in
            while keepaliveOn {
                usleep(100_000)
                guard Server.shared.serving, Server.shared.hasSession, !Server.shared.paused else { continue }
                applyWantedQuality()
                lastLock.lock(); let pb = lastPB; let idle = nowNs() &- lastEncodeNs; let stamp = lastEncodeNs; lastLock.unlock()
                guard let pb else { continue }
                // Sharpen once when the picture settles: the last frame of a change was encoded in a hurry (moving content gets few bits), so after
                // ~0.35 s of stillness send that same frame again, twice, at leisure. Then stay silent: a still desktop costs nothing.
                if stamp != sharpenedStamp && idle > 350_000_000 {
                    statsLock.lock(); let busy = outstanding >= maxInflight; statsLock.unlock()
                    if !busy {
                        if sharpenCount == 0 { sharpenCount = 2 }
                        enc.encode(pb, pts: CMClockGetTime(CMClockGetHostTimeClock()), tCb: nowNs(), capLagUs: 0)
                        sharpenCount -= 1
                        if sharpenCount == 0 { sharpenedStamp = stamp } else { usleep(120_000) }
                    }
                }
                statsLock.lock(); let fk = forceKey, full = outstanding >= maxInflight; statsLock.unlock()
                if full { continue }
                // Only when a fresh picture is needed (new session, resume after pause, keyframe request). NOT periodically:
                // a still desktop must cost the Frame nothing, and the Frame learns the Mac is alive from the connection's own heartbeat.
                if fk && idle > 100_000_000 {
                    lastLock.lock(); lastEncodeNs = nowNs(); lastLock.unlock()
                    enc.encode(pb, pts: CMClockGetTime(CMClockGetHostTimeClock()), tCb: nowNs(), capLagUs: 0)
                }
            }
        }
    }
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sb.isValid,
              let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = arr.first?[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
              let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        gotFrames += 1
        if Server.shared.serving {
            guard Server.shared.hasSession, !Server.shared.paused else { return }       // nobody authenticated (or the Frame's tab is hidden): don't encode or send anything
            statsLock.lock()
            var full = outstanding >= maxInflight
            if full && lastAckNs != 0 && nowNs() - lastAckNs > 250_000_000 {   // acks lost / receiver gone quiet: don't deadlock
                outstanding = 0; forceKey = true; full = false
            }
            if full { skippedByWindow += 1 }
            statsLock.unlock()
            if full { return }
        }
        let tCb = nowNs()
        lastLock.lock(); lastPB = pb; lastEncodeNs = tCb; lastLock.unlock()
        enc.encode(pb, pts: CMSampleBufferGetPresentationTimeStamp(sb), tCb: tCb, capLagUs: 0)
    }
}

/// Starts mirroring the main display at its native pixel size. Returns the running stream + capture objects.
/// Automatic bitrate for a display: about 5 Mbps per megapixel (3440x1440 ~ 25 Mbps, 1080p ~ 10), between 8 and 50 Mbps.
func autoMbps(width: Int, height: Int) -> Int { min(50, max(8, Int((Double(width * height) / 1_000_000 * 5).rounded()))) }

func startCapture(fps: Int, mbps requested: Int = 0) async throws -> (SCStream, Capture) {
    let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
    guard let disp = content.displays.first(where: { $0.displayID == CGMainDisplayID() }) ?? content.displays.first
    else { throw NSError(domain: "capture", code: 1, userInfo: [NSLocalizedDescriptionKey: "no display"]) }
    let mode = CGDisplayCopyDisplayMode(disp.displayID)
    let w = mode?.pixelWidth ?? disp.width, h = mode?.pixelHeight ?? disp.height
    let mbps = requested > 0 ? requested : autoMbps(width: w, height: h)   // 0 = automatic, from this display's size
    print("mirroring display \(disp.displayID): \(w)x\(h) px @\(fps)fps, \(mbps) Mbps HEVC, LL=\(useLL), keyframe \(keySec)s, burst-cap=\(useLimit), window=\(maxInflight)")
    let cfg = SCStreamConfiguration()
    cfg.width = w; cfg.height = h
    cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
    cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    cfg.queueDepth = 5; cfg.showsCursor = true
    let cap = Capture()
    cap.enc = try Encoder(width: w, height: h, fps: fps, bitrate: mbps * 1_000_000)
    let stream = SCStream(filter: SCContentFilter(display: disp, excludingWindows: []), configuration: cfg, delegate: nil)
    try stream.addStreamOutput(cap, type: .screen, sampleHandlerQueue: DispatchQueue(label: "cap"))
    cap.stream = stream; cap.cfg = cfg; cap.baseFps = fps; cap.baseBitrate = mbps * 1_000_000
    cap.startKeepalive()
    try await stream.startCapture()
    return (stream, cap)
}
