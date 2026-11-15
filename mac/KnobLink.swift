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
// It reaches the knob over USB when plugged in, otherwise over Bluetooth, through a
// vendor report carried inside the knob's keyboard. macOS may ask for Input Monitoring
// permission the first time.
//
// It also connects to the SmartKnob Lightroom plugin (if Lightroom is open) and
// passes messages between it and the knob: knob turns -> plugin, slider values and
// the open tool -> knob.

import AppKit
import IOKit.hid
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

// ---------------- Bluetooth link ----------------
// macOS hides Bluetooth keyboards from apps, so instead the knob carries our
// messages as an extra "vendor" report inside its keyboard. The Mac's HID system
// lets us read and write those for one device: 32 bytes per report, lines split
// across reports and padded with zeros.

let LINK_USAGE_PAGE = 0xFF60
let LINK_USAGE = 0x61
let LINK_REPORT_ID: UInt8 = 0x50
let LINK_REPORT_BYTES = 32

final class KnobBluetooth {
    private var manager: IOHIDManager
    private var device: IOHIDDevice?
    private var reportBuffer = [UInt8](repeating: 0, count: 64)
    private var buffer = Data()
    var onLine: ((String) -> Void)?
    var onConnect: (() -> Void)?
    var onDisconnect: (() -> Void)?
    var isOpen: Bool { return device != nil }

    init() {
        manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let match: [String: Any] = [kIOHIDDeviceUsagePageKey as String: LINK_USAGE_PAGE,
                                    kIOHIDDeviceUsageKey as String: LINK_USAGE]
        IOHIDManagerSetDeviceMatching(manager, match as CFDictionary)
        let context = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(manager, { context, _, _, device in
            Unmanaged<KnobBluetooth>.fromOpaque(context!).takeUnretainedValue().attached(device)
        }, context)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, { context, _, _, device in
            Unmanaged<KnobBluetooth>.fromOpaque(context!).takeUnretainedValue().removed(device)
        }, context)
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        let opened = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        if opened != kIOReturnSuccess {
            print("HID: couldn't open the HID system (\(opened)); check Input Monitoring permission")
        }
    }

    private var listed = false
    func tryOpen() {
        // One-time listing of every HID device called SmartKnob, so we can see what the
        // Mac thinks the knob offers
        guard !listed else { return }
        listed = true
        let all = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        IOHIDManagerSetDeviceMatching(all, nil)
        let devices = (IOHIDManagerCopyDevices(all) as? Set<IOHIDDevice>) ?? []
        var lines: [String] = []
        for dev in devices {
            let name = IOHIDDeviceGetProperty(dev, kIOHIDProductKey as CFString) as? String ?? "?"
            guard name.localizedCaseInsensitiveContains("knob") else { continue }
            let page = IOHIDDeviceGetProperty(dev, kIOHIDPrimaryUsagePageKey as CFString) as? Int ?? 0
            let usage = IOHIDDeviceGetProperty(dev, kIOHIDPrimaryUsageKey as CFString) as? Int ?? 0
            let transport = IOHIDDeviceGetProperty(dev, kIOHIDTransportKey as CFString) as? String ?? "?"
            lines.append("\(name) [\(transport)] page 0x\(String(page, radix: 16)) usage 0x\(String(usage, radix: 16))")
        }
        print("HID: knob devices seen: \(lines.isEmpty ? "none" : lines.joined(separator: "; "))")
    }

    private func attached(_ dev: IOHIDDevice) {
        guard device == nil else { return }
        guard IOHIDDeviceOpen(dev, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess else {
            print("Knob (Bluetooth): found but couldn't open it")
            return
        }
        device = dev
        buffer.removeAll()
        let context = Unmanaged.passUnretained(self).toOpaque()
        reportBuffer.withUnsafeMutableBufferPointer { buf in
            IOHIDDeviceRegisterInputReportCallback(dev, buf.baseAddress!, buf.count, { context, _, _, _, reportID, report, length in
                Unmanaged<KnobBluetooth>.fromOpaque(context!).takeUnretainedValue().gotReport(reportID, report, length)
            }, context)
        }
        print("Knob (Bluetooth): connected")
        onConnect?()
    }

    private func removed(_ dev: IOHIDDevice) {
        guard device == dev else { return }
        device = nil
        buffer.removeAll()
        print("Knob (Bluetooth): disconnected")
        onDisconnect?()
    }

    private func gotReport(_ reportID: UInt32, _ report: UnsafeMutablePointer<UInt8>, _ length: Int) {
        guard reportID == UInt32(LINK_REPORT_ID) else { return }
        var bytes = Array(UnsafeBufferPointer(start: report, count: length))
        if bytes.first == LINK_REPORT_ID && bytes.count > LINK_REPORT_BYTES { bytes.removeFirst() }
        buffer.append(contentsOf: bytes.filter { $0 != 0 })   // zeros are padding
        while let newline = buffer.firstIndex(of: 10) {
            let lineData = buffer.subdata(in: buffer.startIndex..<newline)
            buffer.removeSubrange(buffer.startIndex...newline)
            if let line = String(data: lineData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines), !line.isEmpty {
                onLine?(line)
            }
        }
    }

    func sendLine(_ line: String) {
        guard let device = device else { return }
        var data = Array(line.utf8)
        data.append(10)
        var offset = 0
        while offset < data.count {
            let end = min(offset + LINK_REPORT_BYTES, data.count)
            var report = [LINK_REPORT_ID] + Array(data[offset..<end])
            report += [UInt8](repeating: 0, count: LINK_REPORT_BYTES + 1 - report.count)
            let result = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, CFIndex(LINK_REPORT_ID), report, report.count)
            if result != kIOReturnSuccess {
                print("Knob (Bluetooth): write failed (\(result))")
                return
            }
            offset = end
        }
    }

    func send(_ message: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: message),
              let line = String(data: data, encoding: .utf8) else { return }
        sendLine(line)
    }
}

// ---------------- Following the program in front ----------------

final class Link {
    let port = KnobPort()
    let bluetooth = KnobBluetooth()
    // Whichever is connected; USB wins when both are (it's faster and shows logs)
    var knobOpen: Bool { return port.isOpen || bluetooth.isOpen }
    func knobSendLine(_ line: String) {
        if port.isOpen { port.sendLine(line) } else { bluetooth.sendLine(line) }
    }
    func knobSend(_ message: [String: Any]) {
        if port.isOpen { port.send(message) } else { bluetooth.send(message) }
    }
    let toPlugin = SocketChannel(name: "Lightroom plugin (in)", port: LIGHTROOM_TO_PLUGIN_PORT)
    let fromPlugin = SocketChannel(name: "Lightroom plugin (out)", port: LIGHTROOM_FROM_PLUGIN_PORT)
    var knobProfiles: [String] = []   // from the knob's hello
    var knobProfile: String?          // what the knob has open now
    var frontApp: NSRunningApplication?
    var pluginState: [String: String] = [:]   // last "module" / "tool" line from the plugin, replayed to the knob
    var mediaState = ""                        // "playing" / "paused" / "" from Music or Spotify

    init() {
        port.onConnect = { [weak self] in
            self?.knobProfiles = []
            self?.port.send(["t": "hello"])
        }
        port.onLine = { [weak self] line in self?.handle(line) }
        port.onClose = { [weak self] in self?.knobProfiles = []; self?.bluetooth.tryOpen() }
        bluetooth.onConnect = { [weak self] in
            guard let self = self, !self.port.isOpen else { return }
            self.knobProfiles = []
            self.bluetooth.send(["t": "hello"])
        }
        bluetooth.onLine = { [weak self] line in
            guard let self = self, !self.port.isOpen else { return }   // USB is in charge when open
            self.handle(line)
        }
        bluetooth.onDisconnect = { [weak self] in if self?.port.isOpen == false { self?.knobProfiles = [] } }
        // Everything the plugin says goes straight to the knob (values, the open tool)
        fromPlugin.onLine = { [weak self] line in
            if line.contains("\"tool\"") || line.contains("\"module\"") || line.contains("\"meta\"") {
                let key = line.contains("\"tool\"") ? "tool" : (line.contains("\"meta\"") ? "meta" : "module")
                if key != "meta" { print("Lightroom: \(line)") }  // tool / module names, to match the profile
                self?.pluginState[key] = line
            }
            self?.knobSendLine(line)
        }
        fromPlugin.onClose = { [weak self] in self?.pluginState = [:] }

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
            }
            if !self.port.isOpen && !self.bluetooth.isOpen {
                self.bluetooth.tryOpen()
            }
            if self.knobOpen && self.knobProfiles.isEmpty {
                self.knobSend(["t": "hello"])
            }
            self.toPlugin.tryConnect()
            self.fromPlugin.tryConnect()
            self.pollMedia()
        }
        port.tryOpen()
    }

    // Is Music or Spotify playing? (Asked through AppleScript, only if the app is running,
    // so neither gets launched.) Browser audio can't be seen this way.
    func pollMedia() {
        let script = """
        set result to ""
        tell application "System Events" to set names to name of every process
        if names contains "Music" then
            tell application "Music" to set result to (player state as string)
        else if names contains "Spotify" then
            tell application "Spotify" to set result to (player state as string)
        end if
        return result
        """
        DispatchQueue.global().async { [weak self] in
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            task.arguments = ["-e", script]
            let pipe = Pipe()
            task.standardOutput = pipe
            task.standardError = Pipe()
            guard (try? task.run()) != nil else { return }
            task.waitUntilExit()
            let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            let state = out == "playing" ? "playing" : (out == "paused" ? "paused" : "")
            DispatchQueue.main.async {
                guard let self = self, state != self.mediaState else { return }
                self.mediaState = state
                self.knobSend(["t": "media", "state": state])
            }
        }
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
        guard knobOpen, !knobProfiles.isEmpty else { return }
        if knobProfile?.caseInsensitiveCompare(profile) != .orderedSame {
            knobSend(["t": "switch", "p": profile])
            knobProfile = profile
        }
    }

    func handle(_ line: String) {
        // LOG: lines are the knob talking to people (shown); STATE: lines are skipped; messages are JSON
        if line.hasPrefix("LOG: ") {
            print("Knob log: \(line.dropFirst(5))")
            return
        }
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
            // The knob may have missed what Lightroom last reported (it reboots when its
            // port opens): tell it again
            for line in pluginState.values { knobSendLine(line) }
            knobSend(["t": "media", "state": mediaState])
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
