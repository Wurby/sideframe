// Injects the Frame's pointer and keyboard input into the Mac (docs/PROTOCOL.md section 7). Needs the Accessibility permission.
import Foundation
import CoreGraphics
import ApplicationServices

final class InputInjector {
    static let shared = InputInjector()
    var enabled = true                       // the "Allow remote control" menu switch
    var onActivity: (() -> Void)?            // called when input actually arrives (for a status indicator)
    private let lock = NSLock()
    private var pos = CGPoint(x: 0, y: 0)
    private var buttonsDown = Set<Int>()
    private var keysDown = Set<Int>()
    private var modFlags = CGEventFlags()

    var trusted: Bool { AXIsProcessTrusted() }
    func requestTrust() {
        let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(opts)
    }

    // ---- macOS virtual key codes for USB HID usages (page 0x07)
    private static let letters: [Int] = [0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6]   // a...z
    private static let digits: [Int] = [18, 19, 20, 21, 23, 22, 26, 28, 25, 29]                                                       // 1...9, 0
    private static let fkeys: [Int] = [122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111]                                         // F1...F12
    static func macKey(forHID u: Int) -> Int? {
        switch u {
        case 0x04...0x1D: return letters[u - 0x04]
        case 0x1E...0x27: return digits[u - 0x1E]
        case 0x28: return 36   // return
        case 0x29: return 53   // escape
        case 0x2A: return 51   // backspace
        case 0x2B: return 48   // tab
        case 0x2C: return 49   // space
        case 0x2D: return 27; case 0x2E: return 24; case 0x2F: return 33; case 0x30: return 30; case 0x31: return 42
        case 0x33: return 41; case 0x34: return 39; case 0x35: return 50; case 0x36: return 43; case 0x37: return 47; case 0x38: return 44
        case 0x39: return 57   // caps lock
        case 0x3A...0x45: return fkeys[u - 0x3A]
        case 0x4A: return 115; case 0x4B: return 116; case 0x4C: return 117; case 0x4D: return 119; case 0x4E: return 121
        case 0x4F: return 124; case 0x50: return 123; case 0x51: return 125; case 0x52: return 126
        case 0xE0: return 59; case 0xE1: return 56; case 0xE2: return 58; case 0xE3: return 55    // left ctrl, shift, option, command
        case 0xE4: return 62; case 0xE5: return 60; case 0xE6: return 61; case 0xE7: return 54    // right ctrl, shift, option, command
        default: return nil
        }
    }
    private static func flag(forMacKey k: Int) -> CGEventFlags? {
        switch k {
        case 59, 62: return .maskControl
        case 56, 60: return .maskShift
        case 58, 61: return .maskAlternate
        case 55, 54: return .maskCommand
        case 57: return .maskAlphaShift
        default: return nil
        }
    }

    private func post(_ e: CGEvent?) { e?.post(tap: .cghidEventTap) }

    /// message layout: [type][flags][n u16][a u32][b u32] (+ n payload bytes), see PROTOCOL.md
    func handle(type: UInt8, msg m: [UInt8]) {
        guard enabled, trusted, m.count >= 12 else { return }
        let a = UInt32(m[4]) << 24 | UInt32(m[5]) << 16 | UInt32(m[6]) << 8 | UInt32(m[7])
        let b = UInt32(m[8]) << 24 | UInt32(m[9]) << 16 | UInt32(m[10]) << 8 | UInt32(m[11])
        lock.lock(); defer { lock.unlock() }
        onActivity?()
        switch type {
        case 5: // MOVE
            let r = CGDisplayBounds(CGMainDisplayID())
            pos = CGPoint(x: r.minX + r.width * CGFloat(min(a, 1_000_000)) / 1_000_000, y: r.minY + r.height * CGFloat(min(b, 1_000_000)) / 1_000_000)
            let evType: CGEventType = buttonsDown.contains(0) ? .leftMouseDragged : buttonsDown.contains(1) ? .rightMouseDragged : buttonsDown.isEmpty ? .mouseMoved : .otherMouseDragged
            post(CGEvent(mouseEventSource: nil, mouseType: evType, mouseCursorPosition: pos, mouseButton: buttonsDown.contains(1) ? .right : .left))
        case 6: // BUTTON
            let btn = Int(a); let down = b != 0
            let (dt, ut, cgb): (CGEventType, CGEventType, CGMouseButton) = btn == 0 ? (.leftMouseDown, .leftMouseUp, .left) : btn == 1 ? (.rightMouseDown, .rightMouseUp, .right) : (.otherMouseDown, .otherMouseUp, .center)
            if down { buttonsDown.insert(btn) } else { buttonsDown.remove(btn) }
            let e = CGEvent(mouseEventSource: nil, mouseType: down ? dt : ut, mouseCursorPosition: pos, mouseButton: cgb)
            e?.setIntegerValueField(.mouseEventClickState, value: 1)
            post(e)
        case 7: // SCROLL (1/100 line units)
            let dx = Int32(bitPattern: a), dy = Int32(bitPattern: b)
            post(CGEvent(scrollWheelEvent2Source: nil, units: .line, wheelCount: 2, wheel1: dy / 100, wheel2: dx / 100, wheel3: 0))
        case 8: // KEY (HID usage)
            guard let k = InputInjector.macKey(forHID: Int(a)) else { return }
            let down = b != 0
            if down { keysDown.insert(k) } else { keysDown.remove(k) }
            if let f = InputInjector.flag(forMacKey: k) { if down { modFlags.insert(f) } else { modFlags.remove(f) } }
            let e = CGEvent(keyboardEventSource: nil, virtualKey: CGKeyCode(k), keyDown: down)
            e?.flags = modFlags
            post(e)
        case 9: // TEXT (UTF-8 payload)
            let n = Int(m[2]) << 8 | Int(m[3])
            guard n > 0, m.count >= 12 + n, let s = String(bytes: m[12..<(12 + n)], encoding: .utf8) else { return }
            let u = Array(s.utf16)
            for down in [true, false] {
                let e = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: down)
                e?.keyboardSetUnicodeString(stringLength: u.count, unicodeString: u)
                post(e)
            }
        case 10: releaseAllLocked()
        default: break
        }
    }

    /// Let go of every key and button we pressed (session ended / focus lost) so nothing stays stuck on the Mac.
    func releaseAll() { lock.lock(); releaseAllLocked(); lock.unlock() }
    private func releaseAllLocked() {
        for k in keysDown { post(CGEvent(keyboardEventSource: nil, virtualKey: CGKeyCode(k), keyDown: false)) }
        for btn in buttonsDown {
            let (ut, cgb): (CGEventType, CGMouseButton) = btn == 0 ? (.leftMouseUp, .left) : btn == 1 ? (.rightMouseUp, .right) : (.otherMouseUp, .center)
            post(CGEvent(mouseEventSource: nil, mouseType: ut, mouseCursorPosition: pos, mouseButton: cgb))
        }
        keysDown.removeAll(); buttonsDown.removeAll(); modFlags = CGEventFlags()
    }
}
