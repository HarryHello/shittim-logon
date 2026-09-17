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
import MetalKit
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
    var skyLevel: Int = 350         // SkyLight SPACE level for the overlay:
                                    // measured band: 300 and below are covered
                                    // by the lock screen, 320+ float above it
    // The reference stages the whole room live -- flooded floor, water
    // light, sky and stars all render, and the clock is composited INTO
    // the scene -- so nothing is muted by default.
    var mutePatterns = ""

    static func load() -> Config {
        var c = Config()

        // Search relative to the working directory first (interactive runs),
        // then relative to the executable (launchd / double-clicked / a shell
        // that was standing somewhere else entirely). Without the executable
        // fallback a run from build/ silently used every default.
        var candidates: [String] = ["shittim-mac.conf", "macos/shittim-mac.conf"]
        if let exec = Bundle.main.executableURL {
            let dir = exec.deletingLastPathComponent()
            candidates.append(dir.appendingPathComponent("shittim-mac.conf").path)
            // build/ShittimMac -> repo-root/macos/shittim-mac.conf
            candidates.append(dir.appendingPathComponent("../../macos/shittim-mac.conf")
                .standardizedFileURL.path)
        }

        for path in candidates {
            guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { continue }
            say("config: loading \(path)")
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
                case "sky_level": c.skyLevel = Int(v) ?? c.skyLevel
                case "mute_patterns":
                    // An empty value means "keep the default"; "none" is the
                    // explicit way to mute nothing.
                    if v.isEmpty { break }
                    if v.lowercased() == "none" { c.mutePatterns = "" } else { c.mutePatterns = v }
                default: say("config: unknown key '\(k)'")
                }
            }
            // A relative assets directory means relative to the conf's own
            // directory, so the conf stays self-contained no matter where the
            // binary runs from.
            if !c.assets.hasPrefix("/") {
                let confDir = URL(fileURLWithPath: path).deletingLastPathComponent()
                let viaConf = confDir.appendingPathComponent(c.assets).standardizedFileURL.path
                if FileManager.default.fileExists(atPath: viaConf) { c.assets = viaConf }
            }
            return c // first config file found wins
        }
        say("config: no shittim-mac.conf found -- using built-in defaults")
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

    // skyLevel is the SkyLight SPACE level -- the axis that decides whether
    // the overlay floats above the lock screen at all. Measured band: 300
    // and below are covered by the lock screen, 320+ float above everything
    // on it (clock and password field included). Idempotent: one space per
    // process, ever -- bootstrap twice and the first shown space leaks.
    static func bootstrap(spaceLevel: Int32) {
        guard connection == 0 else { return }
        connection = mainConnectionID()
        space = spaceCreate(connection, 1, 0)
        let rc = spaceSetAbsoluteLevel(connection, space, spaceLevel)
        _ = showSpaces(connection, [space] as CFArray)
        say("SkyLight ready: space=\(space) absolute-level=\(spaceLevel) rc=\(rc)")
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

// ------------------------------------------------------------- metal view

final class SceneView: MTKView {
    override var acceptsFirstResponder: Bool { false }
    // MTKView reports itself opaque; the lock-screen overlay needs the
    // password cut-out to blend through to the system UI underneath.
    override var isOpaque: Bool { false }

    override init(frame: CGRect, device: MTLDevice?) {
        super.init(frame: frame, device: device)
        isPaused = true                 // resumed when the scene goes on stage
        enableSetNeedsDisplay = false
        preferredFramesPerSecond = 60
        clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        framebufferOnly = true
        autoResizeDrawable = true
        // CAMetalLayer defaults to opaque compositing; without this the
        // transparent regions render as pure black over the lock screen.
        layer?.isOpaque = false
        layer?.backgroundColor = NSColor.clear.cgColor
    }

    required init(coder: NSCoder) { fatalError("not supported") }
}

final class MetalBox: NSWindow {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

// ------------------------------------------------------------------ engine

final class Engine: NSObject, MTKViewDelegate {
    static let shared = Engine()

    let cfg: Config
    let device: MTLDevice
    let renderer: SceneRenderer
    let window: MetalBox
    let sceneView: SceneView
    var hasCharacter = false
    let debugWake = CommandLine.arguments.contains("--wake")
    var debugFrames = 0

    var mounted = false
    var phaseArrived = false
    var charAlpha: Float = 0
    var roomCharsAlpha: Float = 1
    var wakeT: Float = -1            // seconds since arrival; -1 = not arrived
    var bootT: Float = -1            // seconds since mount; drives the boot materialise
    var fadingOut = false            // unlock fade in progress
    var lastIdle: Double = 999
    var lastFrame = Date().timeIntervalSinceReferenceDate
    var clockMinute = -1
    let clockFormatter: DateFormatter
    let dateFormatter: DateFormatter

    private(set) var windowed = false

    private override init() {
        cfg = Config.load()
        windowed = CommandLine.arguments.contains("--windowed")
        var loadedCharacter = false

        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("no Metal device")
        }
        self.device = device

        guard sb_load_room(cfg.assets, cfg.room) == 0 else {
            fatalError("could not load room '\(cfg.room)' from '\(cfg.assets)'")
        }
        sb_set_mute_patterns(cfg.mutePatterns)
        // Fullscreen fx: the drifting triangle particles (the reference's
        // steady-state layer). Optional -- absent sprite, layer stays off.
        if sb_load_fx((cfg.assets as NSString).appendingPathComponent("FX_TEX_Triangle_02_1.png")) != 0 {
            say("fx: FX_TEX_Triangle_02_1.png not found -- particle layer off")
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
                // The load-time framing measured the setup pose, which for
                // this skeleton is a flat spread, not a standing figure -- a
                // camera on it renders the live pose ~5x too big and leaves
                // her headband in a corner of the screen. Re-solve on the
                // settled animated pose now that the animation is chosen.
                if sb_character_reframe() != 0 {
                    say("character: reframe failed -- phase B keeps the setup-pose framing")
                }
            } else {
                say("character '\(cfg.character)' failed to load -- phase B stays in A")
            }
        }

        let r: SceneRenderer
        do { r = try SceneRenderer(device: device) } catch {
            fatalError("renderer init failed: \(error)")
        }
        renderer = r

        let screenFrame = NSScreen.main!.frame
        let sv = SceneView(frame: NSRect(origin: .zero, size: screenFrame.size),
                           device: device)
        sceneView = sv

        window = MetalBox(contentRect: screenFrame,
                          styleMask: [.borderless],
                          backing: .buffered, defer: false)
        window.contentView = sv
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        // Input is ignored only while the window serves as the lock-screen
        // overlay. The windowed development build stays clickable -- otherwise
        // the dev view cannot even be dragged to the front.
        window.ignoresMouseEvents = cfg.sky && !windowed
        window.collectionBehavior = [.fullScreenAuxiliary, .stationary,
                                     .canJoinAllSpaces, .ignoresCycle]
        window.canBecomeVisibleWithoutLogin = true
        if !windowed {
            // Same number as the sky space level, but a different axis: the
            // space level (Sky.bootstrap) is what lifts the overlay above the
            // lock screen; this window level only orders us within that space.
            window.level = NSWindow.Level(rawValue: cfg.skyLevel)
        }

        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm"   // 24-hour: the scene's own convention
        clockFormatter = formatter
        let dates = DateFormatter()
        dates.setLocalizedDateFormatFromTemplate("EEEE, MMMM d")  // "Friday, September 4"
        dateFormatter = dates
        hasCharacter = loadedCharacter
        super.init()

        sv.delegate = self
        if windowed {
            makeWindowVisibleForDev()
        } else if !cfg.sky {
            say("sky mount disabled (sky=0) -- window stays a plain desktop window")
        }
        // Sky.bootstrap happens once, in main, with the configured space level.
        syncDrawableSize()
    }

    func makeWindowVisibleForDev() {
        window.styleMask.insert([.titled, .closable])
        window.setContentSize(NSSize(width: 1280, height: 720))
        window.center()
        window.level = .normal
        window.makeKeyAndOrderFront(nil)
        if !windowed { return }   // (kept symmetric; windowed is the only caller)
        app.activate(ignoringOtherApps: true)
    }

    func syncDrawableSize() {
        let scale = window.backingScaleFactor
        let px = sceneView.bounds.applying(CGAffineTransform(scaleX: scale, y: scale)).size
        sceneView.drawableSize = px
    }

    // MARK: mount / unmount

    func setMounted(_ m: Bool) {
        guard mounted != m else { return }
        mounted = m
        if m {
            syncDrawableSize()
            window.orderFrontRegardless()
            let rc = Sky.adopt(windowNumber: UInt32(window.windowNumber))
            sceneView.isPaused = false
            // The room's characters materialise in with the white-glow
            // effect (the reference's boot phase), then the scene idles.
            roomCharsAlpha = 0
            bootT = -1
            clockMinute = -1
            // Seed the arrival detector with the REAL idle time: the
            // keystroke that locked the session must not count as an
            // arrival, or the foreground skips the boot phase entirely.
            lastIdle = CGEventSource.secondsSinceLastEventType(.combinedSessionState,
                                                              eventType: anyInput)
            lastFrame = Date().timeIntervalSinceReferenceDate
            say("mounted over lock screen (adopt rc=\(rc))")
        } else {
            // Unlock: the scene dissolves -- a last white glow while the
            // window fades, then everything resets for the next lock.
            fadingOut = true
            phaseArrived = false
            wakeT = -1
            bootT = -1
            sceneView.isPaused = false
            sb_set_character_flash(0.6)
            sb_set_room_chars_flash(0.6)
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.32
                ctx.timingFunction = CAMediaTimingFunction(name: .easeIn)
                window.animator().alphaValue = 0.0
            }, completionHandler: { [weak self] in
                guard let self else { return }
                window.alphaValue = 1.0
                window.orderOut(nil)
                sceneView.isPaused = true
                fadingOut = false
                charAlpha = 0
                roomCharsAlpha = 1
                sb_set_character_alpha(0)
                sb_set_room_chars_alpha(1)
                sb_set_character_flash(0)
                sb_set_room_chars_flash(0)
                say("unmounted (session unlocked)")
            })
        }
    }

    // MARK: MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard mounted || fadingOut || windowed else { return }
        let now = Date().timeIntervalSinceReferenceDate
        var dt = Float(now - lastFrame)
        lastFrame = now
        if dt <= 0 || dt > 0.25 { dt = 1.0 / 60.0 }

        // Boot materialise: the room's characters ignite into the scene with
        // the white-glow effect, then the scene idles (the reference's boot
        // phase, recorded at ~0.7 s into the lock).
        if mounted {
            if bootT < 0 { bootT = 0 }
            if bootT < 0.8 {
                bootT = min(bootT + dt, 0.8)
                roomCharsAlpha = min(1.0, bootT / 0.55)
                sb_set_room_chars_alpha(roomCharsAlpha)
                let f: Float = bootT < 0.12 ? bootT / 0.12
                    : bootT < 0.26 ? 1.0
                    : bootT < 0.75 ? 1.0 - (bootT - 0.26) / 0.49
                    : 0.0
                sb_set_room_chars_flash(f)
            }
        }

        let idle = CGEventSource.secondsSinceLastEventType(.combinedSessionState,
                                                           eventType: anyInput)
        // --wake: debug trigger so the wake timeline can be recorded from
        // the desktop without locking. Fires at frame 150 (~2.5 s in), late
        // enough for a capture burst to be running first.
        if debugWake {
            debugFrames += 1
            if debugFrames == 150 && hasCharacter && !phaseArrived {
                phaseArrived = true
                say("debug wake")
            }
        }
        if !phaseArrived && hasCharacter && lastIdle > 5.0 && idle < 1.0 {
            phaseArrived = true
            say(String(format: "arrival detected (idle %.1f -> %.1f) -- phase B", lastIdle, idle))
        }
        lastIdle = idle

        if phaseArrived && hasCharacter {
            // The wake, as the material stages it: both figures flash white
            // in the same frame, each with its own tint (reference keyframe
            // #23); the foreground resolves from a white silhouette into
            // herself while the room's characters flash and leave.
            if wakeT < 0 { wakeT = 0 }
            wakeT += dt
            let t = wakeT

            // Foreground: alpha fades in over 0.8 s; the flash spikes to
            // full within 0.12 s, holds a beat, then falls away -- a white
            // silhouette that resolves into her colours.
            let newAlpha = min(1.0, charAlpha + dt / 0.8)
            if newAlpha != charAlpha { charAlpha = newAlpha; sb_set_character_alpha(charAlpha) }
            let flash: Float
            if t < 0.12 { flash = t / 0.12 }
            else if t < 0.22 { flash = 1 }
            else if t < 0.57 { flash = 1 - (t - 0.22) / 0.35 }
            else { flash = 0 }
            sb_set_character_flash(flash)

            // Room characters: same flash shape as they leave, fade over 0.8 s.
            let newRoom = max(0.0, roomCharsAlpha - dt / 0.8)
            if newRoom != roomCharsAlpha { roomCharsAlpha = newRoom; sb_set_room_chars_alpha(roomCharsAlpha) }
            let roomFlash: Float
            if t < 0.10 { roomFlash = t / 0.10 }
            else if t < 0.20 { roomFlash = 1 }
            else if t < 0.55 { roomFlash = 1 - (t - 0.20) / 0.35 }
            else { roomFlash = 0 }
            sb_set_room_chars_flash(roomFlash)

            // Latched for the whole mount: the wake plays once per lock,
            // not again on every idle gap while the screen is up.
            if wakeT > 1.0 { wakeT = 1.0 }
        }

        if cfg.clock {
            let minute = Calendar.current.component(.minute, from: Date())
            if minute != clockMinute {
                clockMinute = minute
                let now = Date()
                renderer.clockTexture = renderer.makeClockTexture(
                    time: clockFormatter.string(from: now),
                    date: dateFormatter.string(from: now))
                // Pixel space (the drawable), top-centre, sized from the
                // drawable so the clock keeps its proportions at any scale.
                let dw = sceneView.drawableSize
                let cw = Float(min(880.0, Float(dw.width) * 0.30))
                let ch = cw * 380.0 / 900.0
                renderer.clockRect = CGRect(x: (dw.width - CGFloat(cw)) / 2,
                                            y: CGFloat(dw.height * 0.16),
                                            width: CGFloat(cw), height: CGFloat(ch))
            }
        }

        renderer.draw(layer: view.layer as! CAMetalLayer, dt: dt)
    }

    // MARK: diagnostics

    func dumpWindowState() {
        say("window: visible=\(window.isVisible) occlusion=\(window.occlusionState.rawValue) level=\(window.level.rawValue)")
        say("view: drawable=\(Int(sceneView.drawableSize.width))x\(Int(sceneView.drawableSize.height)) paused=\(sceneView.isPaused) layer=\(String(describing: type(of: sceneView.layer)))")
        say("app: hidden=\(app.isHidden) windowed=\(windowed)")
    }
}

// ------------------------------------------------------------------- main

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let engine = Engine.shared
if engine.windowed {
    engine.makeWindowVisibleForDev()   // dev view: centered, titled, clickable
    engine.sceneView.isPaused = false  // dev view renders continuously
    (engine.sceneView.layer as? CAMetalLayer)?.isOpaque = false
    app.activate(ignoringOtherApps: true)
} else if engine.cfg.sky {
    Sky.bootstrap(spaceLevel: Int32(engine.cfg.skyLevel))
                                       // sky mode: window is already fullscreen;
                                       // it mounts over the lock on lock
} else {
    say("sky mount disabled (sky=0) -- plain desktop window")
}

let watcher = LockWatcher { locked in
    guard engine.cfg.sky, !engine.windowed else {
        say("lock change ignored: mount disabled (--windowed or sky=0)")
        return
    }
    engine.setMounted(locked)
}
watcher.start()

if engine.windowed {
    app.activate(ignoringOtherApps: true)
}

say("ShittimMac ready: room='\(engine.cfg.room)' character='\(engine.cfg.character.isEmpty ? "(none)" : engine.cfg.character)' sky=\(engine.cfg.sky && !engine.windowed) clock=\(engine.cfg.clock)")

DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { engine.dumpWindowState() }

app.run()
