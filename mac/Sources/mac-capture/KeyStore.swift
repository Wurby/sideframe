// Paired Frames and this Mac's identity. Stored in ~/Library/Application Support/Sideframe (directory 0700, files 0600).
import Foundation

struct PairedFrame: Codable {
    var kid: String      // hex of the first 8 bytes of SHA-256(key)
    var name: String     // display name the Frame gave while pairing
    var key: String      // 32-byte pairing key, hex
    var added: Date
}

final class KeyStore {
    static let shared = KeyStore()
    private let lock = NSLock()
    private var frames: [PairedFrame] = []
    private let dir: URL
    private let file: URL
    let macId: Data
    var onChange: (() -> Void)?

    var macName: String { Host.current().localizedName ?? ProcessInfo.processInfo.hostName }

    init() {
        let d = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Sideframe")
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        dir = d; file = d.appendingPathComponent("paired.json")
        let idFile = d.appendingPathComponent("mac-id")
        if let s = try? String(contentsOf: idFile, encoding: .utf8), let id = Data(hex: s.trimmingCharacters(in: .whitespacesAndNewlines)), id.count == 8 {
            macId = id
        } else {
            let id = randomBytes(8)
            KeyStore.writePrivate(Data(id.hex.utf8), to: idFile)
            macId = id
        }
        if let data = try? Data(contentsOf: file), let list = try? JSONDecoder.iso.decode([PairedFrame].self, from: data) { frames = list }
        
    }

    private static func writePrivate(_ data: Data, to url: URL) {
        try? data.write(to: url, options: .atomic)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }

    private func save() {
        guard let data = try? JSONEncoder.iso.encode(frames) else { return }
        KeyStore.writePrivate(data, to: file)
    }

    var all: [PairedFrame] { lock.lock(); defer { lock.unlock() }; return frames }

    func key(forKid kid: String) -> Data? {
        lock.lock(); defer { lock.unlock() }
        return frames.first(where: { $0.kid == kid }).flatMap { Data(hex: $0.key) }
    }
    func name(forKid kid: String) -> String? {
        lock.lock(); defer { lock.unlock() }
        return frames.first(where: { $0.kid == kid })?.name
    }

    @discardableResult func add(name: String, key: Data) -> PairedFrame {
        let f = PairedFrame(kid: keyId(for: key).hex, name: name, key: key.hex, added: Date())
        lock.lock(); frames.removeAll { $0.kid == f.kid }; frames.append(f); save(); lock.unlock()
        onChange?()
        return f
    }
    func remove(kid: String) {
        lock.lock(); frames.removeAll { $0.kid == kid }; save(); lock.unlock()
        onChange?()
    }
}

extension JSONEncoder { static var iso: JSONEncoder { let e = JSONEncoder(); e.dateEncodingStrategy = .iso8601; e.outputFormatting = [.prettyPrinted, .sortedKeys]; return e } }
extension JSONDecoder { static var iso: JSONDecoder { let d = JSONDecoder(); d.dateDecodingStrategy = .iso8601; return d } }
