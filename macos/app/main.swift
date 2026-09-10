// macos/app/main.swift
//
// ShittimMac -- the lock-screen scene, macOS edition. A fullscreen scene
// window that mounts above the ORIGINAL lock screen via a SkyLight space
// (level 400) while the session is locked, and disappears on unlock.
//
//   A (idle)   the room loop plays; nobody there
//   B (wake)   on arrival -- HID idle collapsing after 5+ s -- the foreground
//              character fades in over the room
//
// An in-scene clock replaces the system one (covered by the scene), and a
// transparent cut-out region exposes the system password field.
//
// Safety: attaches only while locked, ignores all input, never becomes
// key/main, no system registration -- quitting the app removes everything.

import AppKit
import Metal
import QuartzCore
import CoreGraphics
import os

let beacon = Logger(subsystem: "local.shittim.mac", category: "app")

func say(_ s: String) {
    FileHandle.standardError.write(("shittim-mac: " + s + "\n").data(using: .utf8)!)
    beacon.notice("\(s, privacy: .public)")
}

/// kCGAnyInputEventType does not import into Swift; ~0 is its value.
let anyInput = CGEventType(rawValue: ~0)!

// ------------------------------------------------------------------ config

struct Config {
    var assets = "../assets"
    var room = "arona_workpage_nighttime_2"
    var idle = 1
    var companion = 11
    var character = ""              // empty = phase B stays in A
    var characterAnim = ""          // empty = first Idle* in the skeleton
    var sky = true
    var clock = true
    var windowed = false
    var hole: SIMD4<Float> = SIMD4(0.36, 0.34, 0.28, 0.50) // fractions: x, y, w, h

    static func load() -> Config {
        var c = Config()
        for path in ["shittim-mac.conf", "macos/shittim-mac.conf"] {
            guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { continue }
            for rawLine in text.split(separator: "\n") {
                let line = rawLine.trimmingCharacters(in: .whitespaces)
                if line.isEmpty || line.hasPrefix("#") { continue }
                let parts = line.split(separator: "=", maxSplits: 1)
                guard parts.count == 2 else { continue }
                let k = parts[0].trimmingCharacters(in: .whitespaces)
                let v = parts[1].trimmingCharacters(in: .whitespaces)
                switch k {
                case "assets": c.assets = v
                case "room": c.room = v
                case "idle": c.idle = Int(v) ?? c.idle
                case "companion": c.companion = Int(v) ?? c.companion
                case "character": c.character = v
                case "character_anim": c.characterAnim = v
                case "sky": c.sky = (v == "1" || v == "true")
                case "clock": c.clock = (v == "1" || v == "true")
                case "hole_x": c.hole.x = Float(v) ?? c.hole.x
                case "hole_y": c.hole.y = Float(v) ?? c.hole.y
                case "hole_w": c.hole.z = Float(v) ?? c.hole.z
                case "hole_h": c.hole.w = Float(v) ?? c.hole.w
                default: say("config: unknown key '\(k)'")
                }
            }
            break // first config file found wins
        }
        return c
    }
}

// ------------------------------------------------- lock + SkyLight mount

enum Sky {
    typealias F_MainConnectionID = @convention(c) () -> Int32
    typealias F_SpaceCreate = @convention(c) (Int32, Int32, Int32) -> Int32
    typealias F_SpaceSetAbsoluteLevel = @convention(c) (Int32, Int32, Int32) -> Int32
    typealias F_ShowSpaces = @convention(c) (Int32, CFArray) -> Int32
    typealias F_SpaceAddWindows = @convention(c) (Int32, Int32, CFArray, Int32) -> Int32

    static let levelNotificationCenterAtScreenLock: Int32 = 400

    static let mainConnectionID: F_MainConnectionID = sym("SLSMainConnectionID")
    static let spaceCreate: F_SpaceCreate = sym("SLSSpaceCreate")
    static let spaceSetAbsoluteLevel: F_SpaceSetAbsoluteLevel = sym("SLSSpaceSetAbsoluteLevel")
    static let showSpaces: F_ShowSpaces = sym("SLSShowSpaces")
    static let spaceAddWindows: F_SpaceAddWindows = sym("SLSSpaceAddWindowsAndRemoveFromSpaces")

    static var connection: Int32 = 0
    static var space: Int32 = 0

    private static func sym<T>(_ name: String) -> T {
        let path = "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight"
        guard let h = dlopen(path, RTLD_NOW), let p = dlsym(h, name) else {
            fatalError("cannot resolve SkyLight symbol \(name)")
        }
        return unsafeBitCast(p, to: T.self)
    }

    static func bootstrap() {
        connection = mainConnectionID()
        space = spaceCreate(connection, 1, 0)
        let rc = spaceSetAbsoluteLevel(connection, space, levelNotificationCenterAtScreenLock)
        _ = showSpaces(connection, [space] as CFArray)
        say("SkyLight ready: space=\(space) level=\(levelNotificationCenterAtScreenLock) rc=\(rc)")
    }

    static func adopt(windowNumber: UInt32) -> Int32 {
        spaceAddWindows(connection, space, [windowNumber] as CFArray, 7)
    }
}

final class LockWatcher {
    private var timer: Timer?
    private var locked = false
    private var onChange: (Bool) -> Void
    init(onChange: @escaping (Bool) -> Void) { self.onChange = onChange }
    func start() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            let now = Self.isSessionLocked()
            guard now != locked else { return }
            locked = now
            say("lock state -> \(locked ? "LOCKED" : "unlocked")")
            onChange(locked)
        }
    }
    static func isSessionLocked() -> Bool {
        guard let d = CGSessionCopyCurrentDictionary() as? [String: Any],
              let v = d["CGSSessionScreenIsLocked"] else { return false }
        if let b = v as? Bool { return b }
        if let i = v as? Int { return i != 0 }
        if let s = v as? String { return s == "1" }
        return false
    }
}

// ------------------------------------------------------------- engine

final class Engine: NSObject {
    static let shared = Engine()

    let cfg: Config
    let device: MTLDevice
    let renderer: SceneRenderer
    let window: MetalBox
    let sceneView: SceneView
    let metalLayer: CAMetalLayer
    var displayLink: CADisplayLink?
    var hasCharacter = false

    var mounted = false
    var phaseArrived = false
    var charAlpha: Float = 0
    var lastIdle: Double = 999
    var lastFrame = Date().timeIntervalSinceReferenceDate
    var clockMinute = -1
    let clockFormatter: DateFormatter

    override init() {
        cfg = Config.load()
        var loadedCharacter = false

        guard sb_load_room(cfg.assets, cfg.room) == 0 else {
            fatalError("could not load room '\(cfg.room)' from '\(cfg.assets)'")
        }
        sb_room_play(0, "Idle_background_00", 1)
        let idleName = String(format: "Idle_%02d", cfg.idle)
        if sb_room_play(1, idleName, 1) != 0 { say("room: no '\(idleName)' -- skipped") }
        if cfg.companion > 0 {
            let cn = String(format: "Idle_%02d", cfg.companion)
            if sb_room_play(4, cn, 1) != 0 { say("room: no '\(cn)' (companion) -- skipped") }
        }

        if !cfg.character.isEmpty {
            if sb_load_character(cfg.assets, cfg.character) == 0 {
                loadedCharacter = true
                let names = (0..<sb_character_animation_count()).map {
                    String(cString: sb_character_animation_name(Int32($0)))
                }
                say("character '\(cfg.character)' loaded; animations: \(names.joined(separator: ", "))")
                if !cfg.characterAnim.isEmpty {
                    if sb_character_play(0, cfg.characterAnim, 1) != 0 {
                        say("character: no '\(cfg.characterAnim)' (set character_anim in config)")
                    }
                } else if let first = names.first(where: { $0.lowercased().contains("idle") }) {
                    sb_character_play(0, first, 1)
                    say("character: playing first Idle* -- '\(first)' (character_anim overrides)")
                }
            } else {
                say("character '\(cfg.character)' failed to load (rc≠0) -- phase B stays in A")
            }
        }

        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("no Metal device")
        }
        self.device = device
        let r: SceneRenderer
        do { r = try SceneRenderer(device: device) } catch {
            fatalError("renderer init failed: \(error)")
        }
        renderer = r

        let screenFrame = NSScreen.main!.frame
        let sv = SceneView(frame: NSRect(origin: .zero, size: screenFrame.size))
        sv.wantsLayer = true
        sceneView = sv
        let ml = sv.layer as! CAMetalLayer
        ml.device = device
        ml.pixelFormat = .bgra8Unorm
        ml.isOpaque = false
        metalLayer = ml

        window = MetalBox(contentRect: screenFrame,
                          styleMask: [.borderless],
                          backing: .buffered, defer: false)
        window.contentView = sv
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.collectionBehavior = [.fullScreenAuxiliary, .stationary,
                                     .canJoinAllSpaces, .ignoresCycle]
        window.canBecomeVisibleWithoutLogin = true
        window.level = NSWindow.Level(rawValue: Int(Int32.max - 2))

        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm"   // 24-hour: the scene's own convention
        clockFormatter = formatter
        hasCharacter = loadedCharacter
        super.init()
    }

    func makeWindowVisibleForDev() {
        window.styleMask.insert([.titled, .closable])
        window.setContentSize(NSSize(width: 1280, height: 720))
        window.center()
        window.makeKeyAndOrderFront(nil)
    }

    func syncDrawableSize() {
        let scale = window.backingScaleFactor
        let px = sceneView.bounds.applying(CGAffineTransform(scaleX: scale, y: scale)).size
        metalLayer.drawableSize = px
        renderer.holeRect = cfg.hole * SIMD4<Float>(Float(px.width), Float(px.height),
                                                    Float(px.width), Float(px.height))
    }

    // MARK: mount / unmount

    func setMounted(_ m: Bool) {
        guard mounted != m else { return }
        mounted = m
        if m {
            syncDrawableSize()
            window.orderFrontRegardless()
            let rc = Sky.adopt(windowNumber: UInt32(window.windowNumber))
            displayLink?.isPaused = false
            lastFrame = Date().timeIntervalSinceReferenceDate
            lastIdle = 999
            say("mounted over lock screen (adopt rc=\(rc))")
        } else {
            displayLink?.isPaused = true
            phaseArrived = false
            charAlpha = 0
            sb_set_character_alpha(0)
            window.orderOut(nil)
            say("unmounted (session unlocked)")
        }
    }

    // MARK: frame

    @objc func tickFrame() {
        guard mounted else { return }
        let now = Date().timeIntervalSinceReferenceDate
        var dt = Float(now - lastFrame)
        lastFrame = now
        if dt <= 0 || dt > 0.25 { dt = 1.0 / 60.0 }

        let idle = CGEventSource.secondsSinceLastEventType(.combinedSessionState,
                                                           eventType: anyInput)
        if !phaseArrived && hasCharacter && lastIdle > 5.0 && idle < 1.0 {
            phaseArrived = true
            say(String(format: "arrival detected (idle %.1f -> %.1f) -- phase B", lastIdle, idle))
        }
        lastIdle = idle

        if phaseArrived && hasCharacter && charAlpha < 1.0 {
            charAlpha = min(1.0, charAlpha + dt / 0.8)
            sb_set_character_alpha(charAlpha)
        }

        if cfg.clock {
            let minute = Calendar.current.component(.minute, from: Date())
            if minute != clockMinute {
                clockMinute = minute
                renderer.clockTexture = renderer.makeTextTexture(
                    text: clockFormatter.string(from: Date()), fontSize: 108)
                let cw = Float(min(720.0, Double(sceneView.bounds.width) * 0.4))
                let ch: Float = 160
                renderer.clockRect = CGRect(x: (sceneView.bounds.width - CGFloat(cw)) / 2,
                                            y: sceneView.bounds.height - CGFloat(ch) - 40,
                                            width: CGFloat(cw), height: CGFloat(ch))
            }
        }

        renderer.draw(layer: metalLayer, dt: dt)
    }
}

// ------------------------------------------------------------ window/view

final class SceneView: NSView {
    override var acceptsFirstResponder: Bool { false }
    override func makeBackingLayer() -> CALayer { CAMetalLayer() }
}

final class MetalBox: NSWindow {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

// ------------------------------------------------------------------- main

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let engine = Engine.shared

if CommandLine.arguments.contains("--windowed") {
    engine.makeWindowVisibleForDev()
} else if engine.cfg.sky {
    Sky.bootstrap()
}

engine.syncDrawableSize()
let engineDisplayLink = engine.sceneView.displayLink(
    target: engine, selector: #selector(Engine.tickFrame))
engineDisplayLink.add(to: .main, forMode: .common)
engineDisplayLink.isPaused = !(engine.cfg.sky && !CommandLine.arguments.contains("--windowed"))

let watcher = LockWatcher { locked in
    guard engine.cfg.sky, !CommandLine.arguments.contains("--windowed") else {
        say("lock change ignored: mount disabled (--windowed or sky=0)")
        return
    }
    engine.setMounted(locked)
}
watcher.start()

say("ShittimMac ready: room='\(engine.cfg.room)' character='\(engine.cfg.character.isEmpty ? "(none)" : engine.cfg.character)' sky=\(engine.cfg.sky && !CommandLine.arguments.contains("--windowed")) clock=\(engine.cfg.clock)")

app.run()
