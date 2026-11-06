// Sideframe — menu-bar app. Your Mac desktop, in your Steam Frame.
// Listens for paired Steam Frames (and advertises itself on the network); while one is connected it mirrors the main display to it and
// injects the Frame's pointer/keyboard input. No Dock icon. Shares Server/Crypto/Capture/Pairing/Input/Stats/Config with the `mac-capture` CLI.
import AppKit
import ScreenCaptureKit
import CryptoKit
import CoreGraphics
import ServiceManagement

let accent = NSColor(calibratedRed: 0.24, green: 0.86, blue: 0.77, alpha: 1)

/// Starts/stops the screen capture so it only runs while a Frame is connected. Always reconciles to the latest wish,
/// so quick connect/disconnect sequences can't leave it in the wrong state.
actor CaptureController {
    private var wanted = false, busy = false
    private var stream: SCStream?
    private var cap: Capture?
    private let onError: @Sendable (String) -> Void
    init(onError: @escaping @Sendable (String) -> Void) { self.onError = onError }

    func setWanted(_ w: Bool) async {
        wanted = w
        if busy { return }
        busy = true
        while (stream != nil) != wanted {
            if wanted {
                do {
                    let (s, c) = try await startCapture(fps: 60)
                    stream = s; cap = c
                } catch {
                    print("capture failed to start: \(error)")
                    onError("\(error)")
                    if "\(error)".contains("3801") { break }                 // permission problem: retrying won't help; the next connection tries again
                    try? await Task.sleep(nanoseconds: 2_000_000_000)        // e.g. "no display" while the screen is off or locked: try again shortly
                    continue
                }
            } else {
                if let s = stream { try? await s.stopCapture() }
                cap?.shutdown()
                stream = nil; cap = nil
                print("capture stopped")
            }
        }
        busy = false
    }
}

enum UIState { case starting, idle, connected(String), needsPermission, error(String) }

// MARK: - small UI helpers
func label(_ text: String, size: CGFloat = 13, weight: NSFont.Weight = .regular, color: NSColor = .labelColor, align: NSTextAlignment = .center, wrap: Bool = true) -> NSTextField {
    let l = NSTextField(wrappingLabelWithString: text)
    l.font = .systemFont(ofSize: size, weight: weight); l.textColor = color; l.alignment = align
    l.isSelectable = false; l.lineBreakMode = wrap ? .byWordWrapping : .byTruncatingTail
    l.maximumNumberOfLines = wrap ? 0 : 1
    return l
}
func appIconImage() -> NSImage? { NSApp.applicationIconImage }
func openSettings(_ pane: String) {
    if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") { NSWorkspace.shared.open(url) }
}

// MARK: - "Pair a Steam Frame" window
final class PairingWindowController: NSWindowController, NSWindowDelegate {
    private let codeLabel = label("– – – –  – – – –", size: 52, weight: .semibold, color: .labelColor)
    private let timeLabel = label("The code stays the same until you close this window or ask for a new one.", size: 11, color: .tertiaryLabelColor)
    private let statusLabel = label("Starting…", size: 14, weight: .medium, color: .secondaryLabelColor)
    private let newCodeButton = NSButton(title: "New Code", target: nil, action: nil)
    private let doneButton = NSButton(title: "Cancel", target: nil, action: nil)
    private var deadline = Date.distantPast
    private var timer: Timer?
    private var finished = false

    init() {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 440), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        w.title = "Pair a Steam Frame"
        super.init(window: w)
        w.delegate = self; w.isReleasedWhenClosed = false
        codeLabel.font = .monospacedDigitSystemFont(ofSize: 52, weight: .semibold)

        let icon = NSImageView(image: appIconImage() ?? NSImage()); icon.imageScaling = .scaleProportionallyUpOrDown
        icon.widthAnchor.constraint(equalToConstant: 72).isActive = true; icon.heightAnchor.constraint(equalToConstant: 72).isActive = true
        let title = label("Pair a Steam Frame", size: 22, weight: .bold)
        let steps = label("On your Steam Frame, open Sideframe and choose “\(KeyStore.shared.macName)”, then enter this code:", size: 13, color: .secondaryLabelColor)
        newCodeButton.target = self; newCodeButton.action = #selector(newCode); newCodeButton.bezelStyle = .rounded
        doneButton.target = self; doneButton.action = #selector(done); doneButton.bezelStyle = .rounded; doneButton.keyEquivalent = "\r"
        let buttons = NSStackView(views: [newCodeButton, doneButton]); buttons.spacing = 12
        let stack = NSStackView(views: [icon, title, steps, codeLabel, timeLabel, statusLabel, buttons])
        stack.orientation = .vertical; stack.alignment = .centerX; stack.spacing = 12
        stack.setCustomSpacing(18, after: steps); stack.setCustomSpacing(4, after: codeLabel); stack.setCustomSpacing(18, after: timeLabel); stack.setCustomSpacing(22, after: statusLabel)
        stack.translatesAutoresizingMaskIntoConstraints = false
        let content = w.contentView!
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 28), stack.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor, constant: -24),
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 36), stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -36),
            steps.widthAnchor.constraint(equalTo: stack.widthAnchor)])
    }
    required init?(coder: NSCoder) { fatalError() }

    func present() {
        PairingManager.shared.onEvent = { [weak self] ev in DispatchQueue.main.async { self?.handle(ev) } }
        finished = false
        PairingManager.shared.begin()
        window?.center(); NSApp.activate(ignoringOtherApps: true); showWindow(nil); window?.makeKeyAndOrderFront(nil)
    }

    private func tick() {}
    private func handle(_ ev: PairEvent) {
        switch ev {
        case .started(let code, let expires):
            deadline = expires; finished = false
            codeLabel.stringValue = PairingManager.display(code); codeLabel.textColor = .labelColor
            statusLabel.stringValue = "Waiting for your Frame…"; statusLabel.textColor = .secondaryLabelColor
            newCodeButton.isEnabled = true; doneButton.title = "Cancel"; timeLabel.stringValue = "The code stays the same until you close this window or ask for a new one."
        case .frameConnecting(let name):
            statusLabel.stringValue = "“\(name)” is pairing…"; statusLabel.textColor = .secondaryLabelColor
        case .wrongCode(let left):
            statusLabel.stringValue = "That code was wrong. \(left) \(left == 1 ? "try" : "tries") left."; statusLabel.textColor = .systemOrange
        case .succeeded(let name):
            finished = true; timeLabel.stringValue = ""
            codeLabel.stringValue = "✓"; codeLabel.textColor = accent
            statusLabel.stringValue = "Paired with “\(name)”.\nYou can close this window."; statusLabel.textColor = .systemGreen
            newCodeButton.isEnabled = false; doneButton.title = "Done"
        case .failed(let why):
            finished = true; timeLabel.stringValue = ""
            codeLabel.stringValue = "– – – –  – – – –"; codeLabel.textColor = .tertiaryLabelColor
            statusLabel.stringValue = why; statusLabel.textColor = .systemRed
            newCodeButton.isEnabled = true; doneButton.title = "Close"
        case .ended: break
        }
    }
    @objc private func newCode() { PairingManager.shared.begin() }
    @objc private func done() { close() }
    func windowWillClose(_ notification: Notification) {
        timer?.invalidate(); timer = nil
        PairingManager.shared.cancel()
    }
}

// MARK: - first-run welcome / setup checklist
final class WelcomeWindowController: NSWindowController {
    private struct Row { let icon: NSImageView; let button: NSButton; let check: () -> Bool; let ok: String; let todo: String; let detail: NSTextField }
    private var rows: [Row] = []
    private var timer: Timer?
    var onPair: (() -> Void)?

    init() {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 470), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        w.title = "Welcome to Sideframe"
        super.init(window: w)
        w.isReleasedWhenClosed = false

        let icon = NSImageView(image: appIconImage() ?? NSImage()); icon.imageScaling = .scaleProportionallyUpOrDown
        icon.widthAnchor.constraint(equalToConstant: 84).isActive = true; icon.heightAnchor.constraint(equalToConstant: 84).isActive = true
        let title = label("Welcome to Sideframe", size: 24, weight: .bold)
        let sub = label("Your Mac desktop, in your Steam Frame.\nThree quick steps and you’re set.", size: 13, color: .secondaryLabelColor)

        func row(_ name: String, _ detail: String, _ button: String, check: @escaping () -> Bool, action: Selector) -> NSView {
            let img = NSImageView(); img.widthAnchor.constraint(equalToConstant: 26).isActive = true; img.heightAnchor.constraint(equalToConstant: 26).isActive = true
            let t = label(name, size: 14, weight: .semibold, align: .left)
            let d = label(detail, size: 12, color: .secondaryLabelColor, align: .left)
            let text = NSStackView(views: [t, d]); text.orientation = .vertical; text.alignment = .leading; text.spacing = 2
            let b = NSButton(title: button, target: self, action: action); b.bezelStyle = .rounded
            let h = NSStackView(views: [img, text, b]); h.orientation = .horizontal; h.alignment = .centerY; h.spacing = 14
            text.setContentHuggingPriority(.defaultLow, for: .horizontal)
            rows.append(Row(icon: img, button: b, check: check, ok: button, todo: button, detail: d))
            return h
        }
        let r1 = row("Allow screen recording", "Lets Sideframe send your Mac’s screen to your Steam Frame.", "Allow…", check: { CGPreflightScreenCaptureAccess() }, action: #selector(allowScreen))
        let r2 = row("Allow accessibility", "Lets your Steam Frame click and type on this Mac. Skip it if you only want to look.", "Allow…", check: { InputInjector.shared.trusted }, action: #selector(allowAccess))
        let r3 = row("Pair your Steam Frame", "Shows a code to enter on your Frame. Do this once.", "Pair…", check: { !KeyStore.shared.all.isEmpty }, action: #selector(pair))
        let finish = NSButton(title: "Finish", target: self, action: #selector(finishTapped)); finish.bezelStyle = .rounded; finish.keyEquivalent = "\r"
        let note = label("If macOS says “Quit & Reopen” after you allow a permission, choose it.", size: 11, color: .tertiaryLabelColor)

        let stack = NSStackView(views: [icon, title, sub, r1, r2, r3, note, finish])
        stack.orientation = .vertical; stack.alignment = .centerX; stack.spacing = 18
        stack.setCustomSpacing(6, after: title); stack.setCustomSpacing(26, after: sub); stack.setCustomSpacing(10, after: r3)
        stack.translatesAutoresizingMaskIntoConstraints = false
        let c = w.contentView!; c.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: c.topAnchor, constant: 28), stack.bottomAnchor.constraint(lessThanOrEqualTo: c.bottomAnchor, constant: -22),
            stack.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: 32), stack.trailingAnchor.constraint(equalTo: c.trailingAnchor, constant: -32)])
        for v in [r1, r2, r3] { v.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true }
        refresh()
    }
    required init?(coder: NSCoder) { fatalError() }

    func present() {
        refresh(); window?.center(); NSApp.activate(ignoringOtherApps: true); showWindow(nil)
        timer?.invalidate(); timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.refresh() }
    }
    private func refresh() {
        for r in rows {
            let ok = r.check()
            let name = ok ? "checkmark.circle.fill" : "circle"
            let img = NSImage(systemSymbolName: name, accessibilityDescription: nil)
            r.icon.image = img; r.icon.contentTintColor = ok ? accent : .tertiaryLabelColor
            r.button.isEnabled = !ok
            r.button.title = ok ? "Done" : r.todo
        }
    }
    @objc private func allowScreen() { if !CGRequestScreenCaptureAccess() { openSettings("Privacy_ScreenCapture") } }
    @objc private func allowAccess() { InputInjector.shared.requestTrust(); if !InputInjector.shared.trusted { openSettings("Privacy_Accessibility") } }
    @objc private func pair() { onPair?() }
    @objc private func finishTapped() { timer?.invalidate(); UserDefaults.standard.set(true, forKey: "didOnboard"); close() }
}

// MARK: - app
@main
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate, NetServiceDelegate {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)      // menu-bar only: no Dock icon
        app.run()
    }

    private var item: NSStatusItem!
    private let menu = NSMenu()
    private var state: UIState = .starting
    private var controller: CaptureController!
    private var bonjour: NetService?
    private var pairingWindow: PairingWindowController?
    private var welcomeWindow: WelcomeWindowController?
    private let logURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/Sideframe.log")

    func applicationDidFinishLaunching(_ notification: Notification) {
        setUpLogging()
        print("=== Sideframe started \(Date())")
        if UserDefaults.standard.object(forKey: "remoteControl") != nil { InputInjector.shared.enabled = UserDefaults.standard.bool(forKey: "remoteControl") }
        controller = CaptureController { [weak self] msg in
            DispatchQueue.main.async {
                let permission = msg.contains("3801") || msg.lowercased().contains("declined") || !CGPreflightScreenCaptureAccess()
                self?.setState(permission ? .needsPermission : .error(msg))
            }
        }
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        menu.delegate = self
        item.menu = menu
        setState(.starting)
        KeyStore.shared.onChange = { [weak self] in DispatchQueue.main.async { self?.advertise() } }

        Server.shared.onSessionStart = { [weak self] in
            resetSessionState()
            DispatchQueue.main.async { self?.setState(.connected(Server.shared.activeFrameName)) }
            Task { await self?.controller.setWanted(true) }
        }
        Server.shared.onSessionEnd = { [weak self] in
            report()
            InputInjector.shared.releaseAll()
            Task { await self?.controller.setWanted(false) }
            DispatchQueue.main.async { self?.setState(.idle) }
        }
        Server.shared.onPauseChanged = { [weak self] in
            let want = Server.shared.hasSession && !Server.shared.paused
            Task { await self?.controller.setWanted(want) }
        }
        do {
            try Server.shared.start(port: 5000)
            print("listening on UDP 5000 — \(KeyStore.shared.all.count) paired Frame(s)")
        } catch {
            print("startup error: \(error)")
            setState(.error("\(error)")); return
        }
        advertise()

        setState(CGPreflightScreenCaptureAccess() ? .idle : .needsPermission)
        if !UserDefaults.standard.bool(forKey: "didOnboard") { showWelcome() }
    }

    // ---- Bonjour: lets the Frame find this Mac without typing an address ----
    private func advertise() {
        bonjour?.stop()
        let svc = NetService(domain: "", type: "_sideframe._udp.", name: KeyStore.shared.macName, port: 5000)
        let txt: [String: Data] = ["v": Data("1".utf8), "id": Data(KeyStore.shared.macId.hex.utf8), "name": Data(KeyStore.shared.macName.utf8)]
        svc.setTXTRecord(NetService.data(fromTXTRecord: txt))
        svc.delegate = self
        svc.publish()
        bonjour = svc
    }
    func netServiceDidPublish(_ sender: NetService) { print("advertising on the network as “\(sender.name)” (_sideframe._udp)") }
    func netService(_ sender: NetService, didNotPublish errorDict: [String: NSNumber]) { print("could not advertise on the network: \(errorDict)") }

    // ---- windows ----
    @objc private func showPairing() {
        if pairingWindow == nil { pairingWindow = PairingWindowController() }
        pairingWindow?.present()
    }
    @objc private func showWelcome() {
        if welcomeWindow == nil { welcomeWindow = WelcomeWindowController(); welcomeWindow?.onPair = { [weak self] in self?.showPairing() } }
        welcomeWindow?.present()
    }
    @objc private func showAbout() {
        NSApp.activate(ignoringOtherApps: true)
        let credits = NSAttributedString(string: "Your Mac desktop, in your Steam Frame.\n\nOpen source under the MIT licence.\nMac and macOS are trademarks of Apple Inc. Steam Frame is a trademark of Valve Corporation. Sideframe is an independent project and is not affiliated with or endorsed by Apple or Valve.",
                                         attributes: [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.secondaryLabelColor])
        NSApp.orderFrontStandardAboutPanel(options: [.applicationName: "Sideframe", .credits: credits])
    }
    @objc private func showLog() { NSWorkspace.shared.open(logURL) }
    @objc private func quitApp() { NSApp.terminate(nil) }

    // ---- permissions / settings toggles ----
    @objc private func allowScreen() { if !CGRequestScreenCaptureAccess() { openSettings("Privacy_ScreenCapture") } }
    @objc private func allowAccess() { InputInjector.shared.requestTrust(); if !InputInjector.shared.trusted { openSettings("Privacy_Accessibility") } }
    @objc private func toggleRemote() {
        InputInjector.shared.enabled.toggle()
        UserDefaults.standard.set(InputInjector.shared.enabled, forKey: "remoteControl")
        if !InputInjector.shared.enabled { InputInjector.shared.releaseAll() }
    }
    @objc private func toggleLogin() {
        let svc = SMAppService.mainApp
        do { if svc.status == .enabled { try svc.unregister() } else { try svc.register() } }
        catch {
            let a = NSAlert(); a.messageText = "Couldn’t change the login item"; a.informativeText = "\(error.localizedDescription)\n\nYou can also add Sideframe in System Settings → General → Login Items."
            a.runModal()
        }
    }
    @objc private func forget(_ sender: NSMenuItem) {
        guard let kid = sender.representedObject as? String, let name = KeyStore.shared.name(forKid: kid) else { return }
        NSApp.activate(ignoringOtherApps: true)
        let a = NSAlert(); a.alertStyle = .warning
        a.messageText = "Forget “\(name)”?"
        a.informativeText = "This Steam Frame will no longer be able to connect to this Mac until you pair it again."
        a.addButton(withTitle: "Forget"); a.addButton(withTitle: "Cancel")
        if a.runModal() == .alertFirstButtonReturn { KeyStore.shared.remove(kid: kid); setState(state) }
    }

    // ---- menu ----
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let status = NSMenuItem(title: statusText(), action: nil, keyEquivalent: ""); status.isEnabled = false
        menu.addItem(status)
        menu.addItem(.separator())

        func add(_ title: String, _ sel: Selector, key: String = "", enabled: Bool = true, state: Bool = false) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: sel, keyEquivalent: key); i.target = self; i.isEnabled = enabled
            if state { i.state = .on }
            menu.addItem(i); return i
        }
        _ = add("Pair a Steam Frame…", #selector(showPairing))

        let frames = KeyStore.shared.all
        let pairedItem = NSMenuItem(title: frames.isEmpty ? "No Frames paired yet" : "Paired Frames", action: nil, keyEquivalent: "")
        if frames.isEmpty { pairedItem.isEnabled = false } else {
            let sub = NSMenu()
            for f in frames {
                let fi = NSMenuItem(title: f.name, action: nil, keyEquivalent: "")
                let fsub = NSMenu(); let forgetItem = NSMenuItem(title: "Forget…", action: #selector(forget(_:)), keyEquivalent: "")
                forgetItem.target = self; forgetItem.representedObject = f.kid; fsub.addItem(forgetItem); fi.submenu = fsub
                sub.addItem(fi)
            }
            pairedItem.submenu = sub
        }
        menu.addItem(pairedItem)
        menu.addItem(.separator())

        let screenOK = CGPreflightScreenCaptureAccess(), accOK = InputInjector.shared.trusted
        _ = add(screenOK ? "Screen recording: allowed" : "Allow screen recording…", #selector(allowScreen), enabled: !screenOK)
        _ = add(accOK ? "Accessibility: allowed" : "Allow accessibility (to click and type)…", #selector(allowAccess), enabled: !accOK)
        _ = add("Allow remote control", #selector(toggleRemote), state: InputInjector.shared.enabled)
        _ = add("Open at login", #selector(toggleLogin), state: SMAppService.mainApp.status == .enabled)
        menu.addItem(.separator())
        _ = add("Setup guide…", #selector(showWelcome))
        _ = add("Show log", #selector(showLog))
        _ = add("About Sideframe", #selector(showAbout))
        _ = add("Quit Sideframe", #selector(quitApp), key: "q")
    }

    private func statusText() -> String {
        switch state {
        case .starting: return "Starting…"
        case .idle: return KeyStore.shared.all.isEmpty ? "Not paired — choose “Pair a Steam Frame…”" : "Ready for your Steam Frame"
        case .connected(let name): return "Connected to \(name)"
        case .needsPermission: return "Needs screen recording permission"
        case .error(let m): return "Problem: \(m.prefix(80))"
        }
    }
    private func setState(_ s: UIState) {
        state = s
        var symbol = "rectangle.righthalf.inset.filled"
        switch s {
        case .connected: symbol = "rectangle.inset.filled"
        case .needsPermission, .error: symbol = "exclamationmark.triangle"
        default: break
        }
        if let img = NSImage(systemSymbolName: symbol, accessibilityDescription: "Sideframe") { img.isTemplate = true; item.button?.image = img }
        else { item.button?.title = "SF" }
        item.button?.toolTip = "Sideframe — \(statusText())"
    }

    // Output goes to ~/Library/Logs/Sideframe.log (there is no terminal); rotated when it gets large.
    private func setUpLogging() {
        let fm = FileManager.default
        try? fm.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let size = (try? fm.attributesOfItem(atPath: logURL.path))?[.size] as? Int, size > 2_000_000 {
            try? fm.removeItem(at: logURL.appendingPathExtension("old"))
            try? fm.moveItem(at: logURL, to: logURL.appendingPathExtension("old"))
        }
        freopen(logURL.path, "a", stdout); freopen(logURL.path, "a", stderr)
        setvbuf(stdout, nil, _IOLBF, 0)
    }
}
