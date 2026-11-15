// KnobLink: tells the knob which program is in front, over the USB cable.
//
// Build:  swiftc -O mac/KnobLink.swift -o mac/knoblink
// Run:    ./mac/knoblink        (Ctrl+C to quit; close the serial monitor first,
//                                only one program can use the knob's USB port)
//
// Each time you switch programs it prints the program's name and ID, e.g.
//   Front: Safari (com.apple.Safari) -> Mac
// Copy an ID into appProfiles below to give that program its own profile.
//
// It also connects to the SmartKnob Lightroom plugin (if Lightroom is open) and
// passes messages between it and the knob: knob turns -> plugin, slider values and
// the open tool -> knob.

import AppKit
import Foundation

// ---------------- Settings ----------------

// Profile used for any program without its own
let defaultProfile = "Mac"

// Program ID -> knob profile name. A program also matches a knob profile
// with exactly its name (e.g. a profile called "Finder").
let appProfiles: [String: String] = [
    "com.adobe.LightroomClassicCC7": "Lightroom",
]

// ---------------- Line channel ----------------
// Anything we read and write one line of text at a time (the knob's USB port, the
// plugin's sockets). Set it up with attach(fd:).

class LineChannel {
    fileprivate var fd: Int32 = -1
    private var source: DispatchSourceRead?
    private var buffer = Data()
    let name: String
    var onLine: ((String) -> Void)?
    var onClose: (() -> Void)?
    var isOpen: Bool { return fd >= 0 }

    init(name: String) { self.name = name }

    func attach(fd f: Int32) {
        _ = fcntl(f, F_SETFL, O_NONBLOCK)
        fd = f
        let s = DispatchSource.makeReadSource(fileDescriptor: f, queue: .main)
        s.setEventHandler { [weak self] in self?.readAvailable() }
        s.setCancelHandler { close(f) }
        source = s
        s.resume()
    }

    private func readAvailable() {
        var bytes = [UInt8](repeating: 0, count: 1024)
        let n = read(fd, &bytes, bytes.count)
        if n <= 0 {
            if n == 0 || (errno != EAGAIN && errno != EINTR) {
                disconnect()
            }
            return
        }
        buffer.append(contentsOf: bytes[0..<n])
        while let newline = buffer.firstIndex(of: 10) {
            let lineData = buffer.subdata(in: buffer.startIndex..<newline)
            buffer.removeSubrange(buffer.startIndex...newline)
            if let line = String(data: lineData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines), !line.isEmpty {
                onLine?(line)
            }
        }
    }

    func disconnect() {
        guard fd >= 0 else { return }
        print("\(name): disconnected")
        source?.cancel()
        source = nil
        fd = -1
        buffer.removeAll()
        onClose?()
    }

    func sendLine(_ line: String) {
        guard fd >= 0 else { return }
        var bytes = Array(line.utf8)
        bytes.append(10)
        if write(fd, bytes, bytes.count) < 0 && errno != EAGAIN {
            disconnect()
        }
    }

    // One message = one line of JSON
    func send(_ message: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: message),
              let line = String(data: data, encoding: .utf8) else { return }
        sendLine(line)
    }
}

// The knob: shows up as /dev/cu.usbmodem...
final class KnobPort: LineChannel {
    var onConnect: (() -> Void)?

    init() { super.init(name: "Knob") }

    func tryOpen() {
        guard !isOpen else { return }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        guard let path = names.filter({ $0.hasPrefix("cu.usbmodem") }).sorted().first.map({ "/dev/" + $0 }) else { return }
        let f = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        if f < 0 {
            return  // busy, e.g. the serial monitor has it; try again later
        }
        var t = termios()
        tcgetattr(f, &t)
        cfmakeraw(&t)
        cfsetspeed(&t, speed_t(B115200))
        t.c_cflag |= tcflag_t(CLOCAL | CREAD)
        tcsetattr(f, TCSANOW, &t)
        attach(fd: f)
        print("Knob: connected on \(path)")
        onConnect?()
    }
}

// The Lightroom plugin: two local sockets, one each way (Lightroom's sockets are one-way)
let LIGHTROOM_TO_PLUGIN_PORT: UInt16 = 58810   // we write here
let LIGHTROOM_FROM_PLUGIN_PORT: UInt16 = 58811 // we read here

final class SocketChannel: LineChannel {
    let port: UInt16
    init(name: String, port: UInt16) {
        self.port = port
        super.init(name: name)
    }

    func tryConnect() {
        guard !isOpen else { return }
        let s = socket(AF_INET, SOCK_STREAM, 0)
        if s < 0 { return }
        var on: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if result != 0 {
            close(s)  // Lightroom (or the plugin) isn't running
            return
        }
        attach(fd: s)
        print("\(name): connected")
    }
}

// ---------------- Following the program in front ----------------

final class Link {
    let port = KnobPort()
    let toPlugin = SocketChannel(name: "Lightroom plugin (in)", port: LIGHTROOM_TO_PLUGIN_PORT)
    let fromPlugin = SocketChannel(name: "Lightroom plugin (out)", port: LIGHTROOM_FROM_PLUGIN_PORT)
    var knobProfiles: [String] = []   // from the knob's hello
    var knobProfile: String?          // what the knob has open now
    var frontApp: NSRunningApplication?

    init() {
        port.onConnect = { [weak self] in
            self?.knobProfiles = []
            self?.port.send(["t": "hello"])
        }
        port.onLine = { [weak self] line in self?.handle(line) }
        // Everything the plugin says goes straight to the knob (values, the open tool)
        fromPlugin.onLine = { [weak self] line in
            if line.contains("\"tool\"") {
                print("Lightroom: \(line)")  // shows tool names, to match profile "tools"
            }
            self?.port.sendLine(line)
        }

        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { [weak self] note in
            self?.frontApp = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            self?.update(log: true)
        }
        frontApp = NSWorkspace.shared.frontmostApplication

        // Every 2 s: connect if the knob isn't, and ask again if it hasn't said hello
        // (opening the port can restart the knob, so the first hello may be missed)
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            if !self.port.isOpen {
                self.port.tryOpen()
            } else if self.knobProfiles.isEmpty {
                self.port.send(["t": "hello"])
            }
            self.toPlugin.tryConnect()
            self.fromPlugin.tryConnect()
        }
        port.tryOpen()
    }

    func knobHas(_ name: String) -> String? {
        return knobProfiles.first { $0.caseInsensitiveCompare(name) == .orderedSame }
    }

    func profileFor(_ app: NSRunningApplication?) -> String {
        if let id = app?.bundleIdentifier, let mapped = appProfiles[id], let p = knobHas(mapped) {
            return p
        }
        if let name = app?.localizedName, let p = knobHas(name) {
            return p
        }
        return defaultProfile
    }

    func update(log: Bool) {
        let profile = profileFor(frontApp)
        if log {
            print("Front: \(frontApp?.localizedName ?? "?") (\(frontApp?.bundleIdentifier ?? "?")) -> \(profile)")
        }
        guard port.isOpen, !knobProfiles.isEmpty else { return }
        if knobProfile?.caseInsensitiveCompare(profile) != .orderedSame {
            port.send(["t": "switch", "p": profile])
            knobProfile = profile
        }
    }

    func handle(_ line: String) {
        // LOG: and STATE: lines are for people; messages are JSON
        guard line.hasPrefix("{"),
              let data = line.data(using: .utf8),
              let message = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let type = message["t"] as? String else { return }

        switch type {
        case "turn", "act":
            toPlugin.sendLine(line)  // for the Lightroom plugin
        case "hello":
            knobProfiles = message["profiles"] as? [String] ?? []
            knobProfile = message["p"] as? String
            print("Knob: firmware \(message["fw"] ?? "?"), profiles \(knobProfiles)")
            update(log: false)
        case "where":
            knobProfile = message["p"] as? String  // also catches changes made on the knob itself
            print("Knob: \(message["p"] ?? "") / \(message["mode"] ?? "") / \(message["page"] ?? "")")
        case "err":
            print("Knob error: \(message["msg"] ?? "")")
        default:
            break
        }
    }
}

let link = Link()
print("KnobLink running (Ctrl+C to quit)")
RunLoop.main.run()
