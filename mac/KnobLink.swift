// KnobLink: tells the knob which program is in front, over the USB cable.
//
// Build:  swiftc -O mac/KnobLink.swift -o mac/knoblink
// Run:    ./mac/knoblink        (Ctrl+C to quit; close the serial monitor first,
//                                only one program can use the knob's USB port)
//
// Each time you switch programs it prints the program's name and ID, e.g.
//   Front: Safari (com.apple.Safari) -> Mac
// Copy an ID into appProfiles below to give that program its own profile.

import AppKit
import Foundation

// ---------------- Settings ----------------

// Profile used for any program without its own
let defaultProfile = "Mac"

// Program ID -> knob profile name. A program also matches a knob profile
// with exactly its name (e.g. a profile called "Finder").
let appProfiles: [String: String] = [
    "com.apple.finder": "Demo",   // test only: Finder opens Demo. Remove once real profiles exist
    // "com.adobe.LightroomClassicCC7": "Lightroom",
]

// ---------------- USB serial port ----------------

final class KnobPort {
    private var fd: Int32 = -1
    private var source: DispatchSourceRead?
    private var buffer = Data()
    var onLine: ((String) -> Void)?
    var onConnect: (() -> Void)?
    var isOpen: Bool { return fd >= 0 }

    // The knob shows up as /dev/cu.usbmodem...
    private func findPort() -> String? {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        return names.filter { $0.hasPrefix("cu.usbmodem") }.sorted().first.map { "/dev/" + $0 }
    }

    func tryOpen() {
        guard fd < 0, let path = findPort() else { return }
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
        fd = f
        print("Knob: connected on \(path)")

        let s = DispatchSource.makeReadSource(fileDescriptor: f, queue: .main)
        s.setEventHandler { [weak self] in self?.readAvailable() }
        s.setCancelHandler { close(f) }
        source = s
        s.resume()
        onConnect?()
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
        print("Knob: disconnected")
        source?.cancel()
        source = nil
        fd = -1
        buffer.removeAll()
    }

    // One message = one line of JSON
    func send(_ message: [String: Any]) {
        guard fd >= 0, let data = try? JSONSerialization.data(withJSONObject: message) else { return }
        var bytes = [UInt8](data)
        bytes.append(10)
        if write(fd, bytes, bytes.count) < 0 && errno != EAGAIN {
            disconnect()
        }
    }
}

// ---------------- Following the program in front ----------------

final class Link {
    let port = KnobPort()
    var knobProfiles: [String] = []   // from the knob's hello
    var knobProfile: String?          // what the knob has open now
    var frontApp: NSRunningApplication?

    init() {
        port.onConnect = { [weak self] in
            self?.knobProfiles = []
            self?.port.send(["t": "hello"])
        }
        port.onLine = { [weak self] line in self?.handle(line) }

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
