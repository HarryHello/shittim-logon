// macos/app/SceneRenderer.swift
//
// The Metal sink. The C++ mechanism layer poses the skeletons and hands over
// pixel-space triangle batches (the "triangle sink" role the adapter README
// predicted); this class owns nothing about the scene -- camera math, layout
// and classification all happened before sb_render.

import Metal
import Foundation
import QuartzCore
import AppKit

final class SceneRenderer {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private var pipelines: [MTLRenderPipelineState] = []
    private var library: MTLLibrary

    private var vertBuffer: MTLBuffer?
    private var indexBuffer: MTLBuffer?
    private var vertCapacity = 0
    private var indexCapacity = 0
    private var textures: [Int: MTLTexture] = [:]
    private var loggedFirstFrame = false

    /// Region (pixels) kept fully transparent so the system password field
    /// shows through the scene. x, y, w, h.
    var holeRect: SIMD4<Float> = [0, 0, 0, 0]
    var clockTexture: MTLTexture?
    var clockRect: CGRect = .zero

    static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct VIn {
        float2 pos [[attribute(0)]];
        float2 uv  [[attribute(1)]];
        uchar4 col [[attribute(2)]];
    };
    struct VOut {
        float4 pos  [[position]];
        float2 px;
        float2 uv;
        float4 col;
    };

    vertex VOut vsMain(VIn in [[stage_in]],
                       constant float2 &screen [[buffer(1)]]) {
        VOut o;
        o.pos = float4(in.pos.x / screen.x * 2.0 - 1.0,
                       1.0 - in.pos.y / screen.y * 2.0, 0.0, 1.0);
        o.px = in.pos;
        o.uv = in.uv;
        o.col = float4(in.col) / 255.0;
        return o;
    }

    fragment float4 fsMain(VOut in [[stage_in]],
                           constant float4 &hole [[buffer(0)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
        if (hole.z > 0.0 && hole.w > 0.0 &&
            in.px.x >= hole.x && in.px.x < hole.x + hole.z &&
            in.px.y >= hole.y && in.px.y < hole.y + hole.w) {
            return float4(0.0);   // password-field cut-out: system UI shows through
        }
        float4 t = tex.sample(smp, in.uv);
        float4 c = t * in.col;    // straight alpha * vertex colour
        c.rgb *= c.a;             // premultiply for the blend states
        return c;
    }

    // The wake's flash, ported from 1.3.0's fx_layer.h. There is no white
    // silhouette: the glow is the figure's silhouette alpha multiplied by a
    // TINT colour (Plana pink->magenta, the room characters white->blue)
    // and added over the frame; a bloom chain spreads it past her outline;
    // FX_TEX outline bursts spawn from her own points (bridge-emitted).
    //
    //   fsGlow: rgb = tint * texel.a -- the glow is sampled from a
    //     silhouette texture's alpha, never from its colours.
    //   fsSnap: a PREMULTIPLIED texture drawn back with the whole image
    //     scaled by the vertex alpha -- the exit's frozen dissolve.
    //   fsCopy: passthrough, for the bloom's downsample chain.
    fragment float4 fsGlow(VOut in [[stage_in]],
                           constant float4 &tint [[buffer(1)]],
                           constant float4 &hole [[buffer(0)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
        if (hole.z > 0.0 && hole.w > 0.0 &&
            in.px.x >= hole.x && in.px.x < hole.x + hole.z &&
            in.px.y >= hole.y && in.px.y < hole.y + hole.w) {
            return float4(0.0);
        }
        float4 t = tex.sample(smp, in.uv);
        float a = t.a * in.col.a;
        return float4(tint.rgb * a, a);
    }

    fragment float4 fsSnap(VOut in [[stage_in]],
                           constant float4 &hole [[buffer(0)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
        if (hole.z > 0.0 && hole.w > 0.0 &&
            in.px.x >= hole.x && in.px.x < hole.x + hole.z &&
            in.px.y >= hole.y && in.px.y < hole.y + hole.w) {
            return float4(0.0);
        }
        float4 t = tex.sample(smp, in.uv);
        return t * in.col.a;      // the flash envelope lives in the vertex alpha
    }

    fragment float4 fsCopy(VOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
        return tex.sample(smp, in.uv);
    }
    """

    init(device: MTLDevice) throws {
        self.device = device
        guard let queue = device.makeCommandQueue() else {
            throw NSError(domain: "SceneRenderer", code: 1)
        }
        self.queue = queue
        library = try device.makeLibrary(source: Self.shaderSource, options: nil)

        let vd = MTLVertexDescriptor()
        vd.attributes[0].format = .float2
        vd.attributes[0].offset = 0
        vd.attributes[0].bufferIndex = 0
        vd.attributes[1].format = .float2
        vd.attributes[1].offset = 8
        vd.attributes[1].bufferIndex = 0
        vd.attributes[2].format = .uchar4   // raw 0..255; the shader divides by 255
        vd.attributes[2].offset = 16
        vd.attributes[2].bufferIndex = 0
        vd.layouts[0].stride = 20

        let modes: [(MTLBlendFactor, MTLBlendFactor, MTLBlendFactor, MTLBlendFactor)] = [
            (.one, .oneMinusSourceAlpha, .one, .oneMinusSourceAlpha),      // normal
            (.one, .one, .one, .oneMinusSourceAlpha),                      // additive
            (.destinationColor, .oneMinusSourceAlpha, .one, .oneMinusSourceAlpha), // multiply
            (.one, .oneMinusDestinationColor, .one, .oneMinusSourceAlpha), // screen
        ]
        for m in modes {
            let pd = MTLRenderPipelineDescriptor()
            pd.vertexFunction = library.makeFunction(name: "vsMain")
            pd.fragmentFunction = library.makeFunction(name: "fsMain")
            pd.vertexDescriptor = vd
            pd.colorAttachments[0].pixelFormat = .bgra8Unorm
            pd.colorAttachments[0].isBlendingEnabled = true
            pd.colorAttachments[0].rgbBlendOperation = .add
            pd.colorAttachments[0].alphaBlendOperation = .add
            pd.colorAttachments[0].sourceRGBBlendFactor = m.0
            pd.colorAttachments[0].destinationRGBBlendFactor = m.1
            pd.colorAttachments[0].sourceAlphaBlendFactor = m.2
            pd.colorAttachments[0].destinationAlphaBlendFactor = m.3
            pipelines.append(try device.makeRenderPipelineState(descriptor: pd))
        }
        // Blend index 4: the flash glow (fsGlow, additive both). The tint
        // arrives per draw as fragment bytes; the silhouette alpha comes
        // from the sampled texture.
        let glowP = MTLRenderPipelineDescriptor()
        glowP.vertexFunction = library.makeFunction(name: "vsMain")
        glowP.fragmentFunction = library.makeFunction(name: "fsGlow")
        glowP.vertexDescriptor = vd
        glowP.colorAttachments[0].pixelFormat = .bgra8Unorm
        glowP.colorAttachments[0].isBlendingEnabled = true
        glowP.colorAttachments[0].rgbBlendOperation = .add
        glowP.colorAttachments[0].alphaBlendOperation = .add
        glowP.colorAttachments[0].sourceRGBBlendFactor = .one
        glowP.colorAttachments[0].destinationRGBBlendFactor = .one
        glowP.colorAttachments[0].sourceAlphaBlendFactor = .one
        glowP.colorAttachments[0].destinationAlphaBlendFactor = .one
        pipelines.append(try device.makeRenderPipelineState(descriptor: glowP))
        // Blend index 5: the frozen dissolve (fsSnap, normal blending) --
        // the snapshot composited over the frame, whole image scaled by
        // the flash envelope.
        let snapP = MTLRenderPipelineDescriptor()
        snapP.vertexFunction = library.makeFunction(name: "vsMain")
        snapP.fragmentFunction = library.makeFunction(name: "fsSnap")
        snapP.vertexDescriptor = vd
        snapP.colorAttachments[0].pixelFormat = .bgra8Unorm
        snapP.colorAttachments[0].isBlendingEnabled = true
        snapP.colorAttachments[0].rgbBlendOperation = .add
        snapP.colorAttachments[0].alphaBlendOperation = .add
        snapP.colorAttachments[0].sourceRGBBlendFactor = .one
        snapP.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        snapP.colorAttachments[0].sourceAlphaBlendFactor = .one
        snapP.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        pipelines.append(try device.makeRenderPipelineState(descriptor: snapP))
        // Blend index 6: passthrough copy (fsCopy, blending off) -- the
        // bloom chain's downsample steps.
        let copyP = MTLRenderPipelineDescriptor()
        copyP.vertexFunction = library.makeFunction(name: "vsMain")
        copyP.fragmentFunction = library.makeFunction(name: "fsCopy")
        copyP.vertexDescriptor = vd
        copyP.colorAttachments[0].pixelFormat = .bgra8Unorm
        copyP.colorAttachments[0].isBlendingEnabled = false
        pipelines.append(try device.makeRenderPipelineState(descriptor: copyP))
    }

    // MARK: textures

    func texture(forPage page: Int) -> MTLTexture? {
        if let t = textures[page] { return t }
        var rgba: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0
        guard sb_page(Int32(page), &rgba, &w, &h) == 0, let rgba, w > 0, h > 0 else { return nil }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm,
                                                            width: Int(w), height: Int(h),
                                                            mipmapped: false)
        desc.usage = [.shaderRead]
        guard let t = device.makeTexture(descriptor: desc) else { return nil }
        let region = MTLRegion(origin: MTLOrigin(x: 0, y: 0, z: 0),
                               size: MTLSize(width: Int(w), height: Int(h), depth: 1))
        t.replace(region: region, mipmapLevel: 0, withBytes: rgba, bytesPerRow: Int(w) * 4)
        textures[page] = t
        return t
    }

    /// The in-scene clock, staged like the reference: big 24-hour time with
    /// the localized date under it, both centred in a 900x380 texture.
    func makeClockTexture(time: String, date: String) -> MTLTexture? {
        let W = 900, H = 380
        let img = NSImage(size: NSSize(width: W, height: H))
        img.lockFocus()
        let timeAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 168, weight: .light),
            .foregroundColor: NSColor.white,
        ]
        let dateAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 46, weight: .medium),
            .foregroundColor: NSColor.white,
        ]
        let tSize = (time as NSString).size(withAttributes: timeAttrs)
        let dSize = (date as NSString).size(withAttributes: dateAttrs)
        (time as NSString).draw(at: NSPoint(x: (CGFloat(W) - tSize.width) / 2,
                                            y: CGFloat(H) - tSize.height - 16),
                                withAttributes: timeAttrs)
        (date as NSString).draw(at: NSPoint(x: (CGFloat(W) - dSize.width) / 2,
                                            y: CGFloat(H) - tSize.height - 16
                                                - dSize.height - 12),
                                withAttributes: dateAttrs)
        img.unlockFocus()
        guard let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }

        var px = [UInt8](repeating: 0, count: W * H * 4)
        let cs = CGColorSpaceCreateDeviceRGB()
        guard let ctx = CGContext(data: &px, width: W, height: H, bitsPerComponent: 8,
                                  bytesPerRow: W * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        ctx.interpolationQuality = .none
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: W, height: H))

        for i in stride(from: 3, to: px.count, by: 4) {
            let a = px[i]
            if a != 0 && a != 255 {
                for c in 0..<3 { px[i - 3 + c] = UInt8((UInt16(px[i - 3 + c]) * 255 + UInt16(a) / 2) / UInt16(a)) }
            }
        }

        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm,
                                                            width: W, height: H, mipmapped: false)
        desc.usage = [.shaderRead]
        guard let t = device.makeTexture(descriptor: desc) else { return nil }
        t.replace(region: MTLRegion(origin: MTLOrigin(x: 0, y: 0, z: 0),
                                    size: MTLSize(width: W, height: H, depth: 1)),
                  mipmapLevel: 0, withBytes: px, bytesPerRow: W * 4)
        return t
    }

    // MARK: flash machinery

    /// Set by the engine for ONE frame: 1 = bake the room's exit snapshot
    /// (sb_snapshot kind 0) into snapRoomTex, 2 = the foreground's
    /// silhouette (kind 1) into snapCharTex.
    var captureRequest = 0

    private var snapRoomTex: MTLTexture?   // the exit's dissolve image (whole room)
    private var silRoomTex: MTLTexture?    // the room's characters only -- its glow's silhouette
    private var snapCharTex: MTLTexture?
    private var silTex: MTLTexture?        // the live silhouette (arrival/boot glow)
    private var q1: [MTLTexture] = []      // bloom level 1, one per glow slot
    private var q2: [MTLTexture] = []      // bloom level 2, one per glow slot
    private var chainW = 0, chainH = 0

    private func offscreen(_ w: Int, _ h: Int) -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                        width: w, height: h, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        return device.makeTexture(descriptor: d)!
    }

    private func rebuildChain(_ w: Int, _ h: Int) {
        if chainW == w, chainH == h, snapRoomTex != nil { return }
        chainW = w; chainH = h
        snapRoomTex = offscreen(w, h)
        silRoomTex = offscreen(w, h)
        snapCharTex = offscreen(w, h)
        silTex = offscreen(w, h)
        // Three glow sources can overlap (exit runs 0.5s, the enter flash
        // lights 0.1s before it ends), so each slot owns its bloom chain.
        q1 = (0..<3).map { _ in offscreen(max(1, w / 4), max(1, h / 4)) }
        q2 = (0..<3).map { _ in offscreen(max(1, w / 8), max(1, h / 8)) }
    }

    private struct QuadVert { var x, y, u, v: Float; var r, g, b, a: UInt8 }

    private func fullscreenQuad(_ w: Float, _ h: Float, alpha: UInt8 = 255) -> [QuadVert] {
        [QuadVert(x: 0, y: 0, u: 0, v: 0, r: 255, g: 255, b: 255, a: alpha),
         QuadVert(x: w,   y: 0, u: 1, v: 0, r: 255, g: 255, b: 255, a: alpha),
         QuadVert(x: 0,   y: h, u: 0, v: 1, r: 255, g: 255, b: 255, a: alpha),
         QuadVert(x: w,   y: h, u: 1, v: 1, r: 255, g: 255, b: 255, a: alpha)]
    }

    /// Renders a batch list into a render target with the spine pipelines
    /// (0..3), cleared first. Shared by the snapshot bake and the live
    /// silhouette render. The buffers are passed in because the bake
    /// executes only at commit time: whatever they hold then is what gets
    /// drawn, so a bake never shares a buffer the live path rewrites.
    private func bakeBatches(_ cmd: MTLCommandBuffer,
                             tex: MTLTexture, w: Int, h: Int,
                             vertBuf: MTLBuffer, idxBuf: MTLBuffer,
                             batches: UnsafePointer<SBBatch>, batchCount: Int,
                             passFilter: ((Int) -> Bool)? = nil) {
        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = tex
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].storeAction = .store
        guard let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
        var hole = SIMD4<Float>(0, 0, 0, 0)   // the bake is full-frame
        enc.setFragmentBytes(&hole, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
        let smp = device.makeSamplerState(descriptor: Self.samplerDesc())
        enc.setFragmentSamplerState(smp, index: 0)
        var screen = SIMD2<Float>(Float(w), Float(h))
        enc.setVertexBytes(&screen, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
        enc.setVertexBuffer(vertBuf, offset: 0, index: 0)
        for i in 0..<batchCount {
            let b = batches[i]
            if let f = passFilter, !f(Int(b.pass)) { continue }
            guard let tex = texture(forPage: Int(b.page)) else { continue }
            enc.setRenderPipelineState(pipelines[min(max(Int(b.blend), 0), 3)])
            enc.setFragmentTexture(tex, index: 0)
            enc.drawIndexedPrimitives(type: .triangle, indexCount: Int(b.indexCount),
                                      indexType: .uint16, indexBuffer: idxBuf,
                                      indexBufferOffset: Int(b.indexOffset) * 2)
        }
        enc.endEncoding()
    }

    private static func samplerDesc() -> MTLSamplerDescriptor {
        let d = MTLSamplerDescriptor()
        d.minFilter = .linear
        d.magFilter = .linear
        d.sAddressMode = .clampToEdge
        d.tAddressMode = .clampToEdge
        return d
    }

        /// One downsample step (fsCopy, blending off, linear): `src` drawn
    /// stretched over `dst`.
    private func copyInto(_ cmd: MTLCommandBuffer, src: MTLTexture, dst: MTLTexture) {
        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = dst
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].storeAction = .store
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        guard let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
        enc.setRenderPipelineState(pipelines[6])
        enc.setFragmentTexture(src, index: 0)
        let smp = device.makeSamplerState(descriptor: Self.samplerDesc())
        enc.setFragmentSamplerState(smp, index: 0)
        var screen = SIMD2<Float>(Float(dst.width), Float(dst.height))
        enc.setVertexBytes(&screen, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
        let quad = fullscreenQuad(Float(dst.width), Float(dst.height))
        enc.setVertexBytes(quad, length: 4 * 20, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
    }

    // MARK: frame

    /// dt: real frame delta, already fed to sb_tick by the caller.
    func draw(layer: CAMetalLayer, dt: Float) {
        let w = Int(layer.drawableSize.width)
        let h = Int(layer.drawableSize.height)
        guard w > 0, h > 0 else { return }

        var verts: UnsafePointer<SBBatchVertex>?
        var indices: UnsafePointer<UInt16>?
        var batches: UnsafePointer<SBBatch>?
        var vertCount: Int32 = 0, indexCount: Int32 = 0, batchCount: Int32 = 0

        sb_tick(dt)
        guard let drawable = layer.nextDrawable() else { return }
        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = drawable.texture
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        rpd.colorAttachments[0].loadAction = .clear
        rpd.colorAttachments[0].storeAction = .store

        sb_render(Int32(w), Int32(h), &verts, &vertCount, &indices, &indexCount,
                  &batches, &batchCount)
        guard batchCount > 0, let sv = verts, let si = indices, let sb = batches else {
            if let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) {
                enc.endEncoding()
                cmd.present(drawable)
                cmd.commit()
            }
            return
        }

        if !loggedFirstFrame {
            loggedFirstFrame = true
            say("first frame: batches=\(batchCount) verts=\(vertCount) indices=\(indexCount) pages=\(sb_page_count())")
        }
        ensureCapacity(vertices: Int(vertCount), indices: Int(indexCount))
        vertBuffer?.contents().copyMemory(from: sv, byteCount: Int(vertCount) * 20)
        indexBuffer?.contents().copyMemory(from: si, byteCount: Int(indexCount) * 2)
        rebuildChain(w, h)

        guard let cmd = queue.makeCommandBuffer() else { return }

        // ---- the flash machinery's offscreen passes, in order before the
        // main canvas so it sees finished textures. One shared staging
        // buffer: the snapshot bake copies its own geometry in first, the
        // live geometry is copied back before its own pass.
        if captureRequest != 0 {
            let kind = captureRequest == 2 ? 1 : 0
            // Own command buffer, committed before the main one: the queue
            // runs buffers in commit order, so the canvas below samples the
            // finished bakes. (One buffer for both would also be fine order-
            // wise, but the bakes must not read buffers the live path
            // rewrites -- see bakeBatches.)
            guard let capCmd = queue.makeCommandBuffer() else { return }
            // The exit bakes TWO images from one begin: the whole room
            // (the dissolve) and the characters alone (the glow's
            // silhouette -- sb_snapshot kind 2).
            for snapKind: Int32 in kind == 1 ? [1] : [0, 2] {
                var snapVerts: UnsafePointer<SBBatchVertex>?
                var snapIdx: UnsafePointer<UInt16>?
                var snapBatches: UnsafePointer<SBBatch>?
                var nv: Int32 = 0, ni: Int32 = 0, nb: Int32 = 0
                if sb_snapshot(snapKind, &snapVerts, &nv, &snapIdx, &ni, &snapBatches, &nb) == 0,
                   let svb = snapVerts, let sib = snapIdx, let sbb = snapBatches {
                    // Fresh staging per bake: private to this bake's
                    // execution, the live path never touches it.
                    let vbuf = device.makeBuffer(length: Int(nv) * 20,
                                                 options: [.storageModeShared])!
                    vbuf.contents().copyMemory(from: svb, byteCount: Int(nv) * 20)
                    let ibuf = device.makeBuffer(length: Int(ni) * 2,
                                                 options: [.storageModeShared])!
                    ibuf.contents().copyMemory(from: sib, byteCount: Int(ni) * 2)
                    bakeBatches(capCmd, tex: snapKind == 0 ? snapRoomTex!
                                    : snapKind == 1 ? snapCharTex! : silRoomTex!,
                                w: w, h: h, vertBuf: vbuf, idxBuf: ibuf,
                                batches: sbb, batchCount: Int(nb))
                } else {
                    say("exit snapshot kind \(snapKind) EMPTY")
                }
            }
            capCmd.commit()
            if ProcessInfo.processInfo.environment["SHITTIM_TEX_DUMP"] != nil {
                // Debug: what the GPU actually baked (each dump
                // self-synchronizes behind capCmd on the queue).
                if let t = snapRoomTex { dumpTexture(t, "/tmp/gpu_snapRoom.png") }
                if let t = silRoomTex { dumpTexture(t, "/tmp/gpu_silRoom.png") }
                if let t = snapCharTex { dumpTexture(t, "/tmp/gpu_snapChar.png") }
            }
            captureRequest = 0
        }
        // The arrival/boot glow samples the live silhouette: the room's
        // character batches, re-rendered every frame so it tracks their ramp.
        let roomCharsLive = sb_fx_active(2) == 1
        if roomCharsLive, let vt = vertBuffer, let it = indexBuffer {
            bakeBatches(cmd, tex: silTex!, w: w, h: h, vertBuf: vt, idxBuf: it,
                        batches: sb, batchCount: Int(batchCount)) { $0 == 1 }
        }
        // The bloom chain per active glow source: quarter, then eighth --
        // the stand-in for upstream's four-level stack, giving the halo its
        // spread past the silhouette.
        struct GlowSource { let kind: Int32; let tex: MTLTexture; let slot: Int }
        var glows: [GlowSource] = []
        let noGlow = ProcessInfo.processInfo.environment["SHITTIM_NO_GLOW"] != nil
        if sb_fx_active(0) == 1, !noGlow, let t = silRoomTex { glows.append(GlowSource(kind: 0, tex: t, slot: 0)) }
        if sb_fx_active(1) == 1, !noGlow, let t = snapCharTex { glows.append(GlowSource(kind: 1, tex: t, slot: 1)) }
        if roomCharsLive, !noGlow, let t = silTex { glows.append(GlowSource(kind: 2, tex: t, slot: 2)) }
        for g in glows {
            copyInto(cmd, src: g.tex, dst: q1[g.slot])
            copyInto(cmd, src: q1[g.slot], dst: q2[g.slot])
        }

        // ---- the main canvas
        guard let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }

        var screen = SIMD2<Float>(Float(w), Float(h))
        enc.setVertexBytes(&screen, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
        var hole = holeRect
        enc.setFragmentBytes(&hole, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
        let smp = device.makeSamplerState(descriptor: Self.samplerDesc())
        enc.setFragmentSamplerState(smp, index: 0)

        enc.setVertexBuffer(vertBuffer, offset: 0, index: 0)

        var clockDrawn = false
        var snapshotDrawn = false
        func drawQuad(_ quad: [QuadVert], pipeline: Int, tex: MTLTexture) {
            enc.setRenderPipelineState(pipelines[pipeline])
            enc.setFragmentTexture(tex, index: 0)
            enc.setVertexBytes(quad, length: 4 * 20, index: 0)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            // setVertexBytes replaced the binding at index 0 for the whole
            // encoder: every batch after this would otherwise render with
            // these four vertices -- the foreground figure vanished exactly
            // this way once. Re-bind.
            if let vertBuffer { enc.setVertexBuffer(vertBuffer, offset: 0, index: 0) }
        }
        // The exit's frozen room: composited before the clock and the
        // foreground, scaled by the flash envelope (fsSnap).
        func drawExitSnapshot() {
            guard sb_fx_active(0) == 1, let t = snapRoomTex else { return }
            let a = UInt8(min(255.0, sb_fx_mag(0) * 255.0 + 0.5))
            drawQuad(fullscreenQuad(Float(w), Float(h), alpha: a), pipeline: 5, tex: t)
        }
        func drawClock() {
            guard let clockTexture, clockRect.width > 0 else { return }
            let cw = Float(clockRect.width), ch = Float(clockRect.height)
            let x0 = Float(clockRect.minX), y0 = Float(clockRect.minY)
            let quad: [QuadVert] = [
                QuadVert(x: x0,      y: y0,      u: 0, v: 0, r: 255, g: 255, b: 255, a: 255),
                QuadVert(x: x0 + cw, y: y0,      u: 1, v: 0, r: 255, g: 255, b: 255, a: 255),
                QuadVert(x: x0,      y: y0 + ch, u: 0, v: 1, r: 255, g: 255, b: 255, a: 255),
                QuadVert(x: x0 + cw, y: y0 + ch, u: 1, v: 1, r: 255, g: 255, b: 255, a: 255),
            ]
            drawQuad(quad, pipeline: 0, tex: clockTexture)
        }

        let exitActive = sb_fx_active(0) == 1
        for i in 0..<Int(batchCount) {
            let b = sb[i]
            let page = Int(b.page)
            let blend = Int(b.blend)
            let pass = Int(b.pass)
            let indexOffset = Int(b.indexOffset)
            let indexCount = Int(b.indexCount)
            // While the exit flash runs, the frozen snapshot replaces the
            // live room (stage and characters both): nothing moves while
            // it dissolves.
            if exitActive && pass <= 1 { continue }
            if pass >= 2 {
                if !snapshotDrawn { drawExitSnapshot(); snapshotDrawn = true }
                if !clockDrawn {
                    drawClock()
                    clockDrawn = true
                }
            }
            guard page < Int(sb_page_count()), let tex = texture(forPage: page) else { continue }
            // 0..3 the spine blend modes; 4 fsGlow; 5 fsSnap; 6 fsCopy.
            enc.setRenderPipelineState(pipelines[min(max(blend, 0), 3)])
            enc.setFragmentTexture(tex, index: 0)
            enc.drawIndexedPrimitives(type: .triangle, indexCount: indexCount,
                                      indexType: .uint16, indexBuffer: indexBuffer!,
                                      indexBufferOffset: indexOffset * 2)
        }
        if !snapshotDrawn { drawExitSnapshot() }
        if !clockDrawn { drawClock() }

        // ---- the flashes' glow, over everything, additive: the crisp
        // silhouette term (the accumulated fxAdd) plus the bloom stand-in
        // (the two downsampled levels, weights per upstream's
        // strength/levels). The tint arrives per draw as fragment bytes.
        for g in glows {
            var c = [Float](repeating: 0, count: 3)
            sb_fx_color(g.kind, &c)
            func glowDraw(_ tex: MTLTexture, weight: Float) {
                enc.setRenderPipelineState(pipelines[4])
                enc.setFragmentTexture(tex, index: 0)
                var t = SIMD4<Float>(c[0] * weight, c[1] * weight, c[2] * weight, 1)
                enc.setFragmentBytes(&t, length: MemoryLayout<SIMD4<Float>>.stride, index: 1)
                let quad = fullscreenQuad(Float(w), Float(h))
                enc.setVertexBytes(quad, length: 4 * 20, index: 0)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                if let vertBuffer { enc.setVertexBuffer(vertBuffer, offset: 0, index: 0) }
            }
            glowDraw(g.tex, weight: 1.0)      // the AccumulateGlow core
            glowDraw(q1[g.slot], weight: 0.25)  // bloom level 1
            glowDraw(q2[g.slot], weight: 0.125) // bloom level 2
        }

        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }

    private func ensureCapacity(vertices: Int, indices: Int) {
        if vertCapacity < vertices {
            vertCapacity = max(vertices * 2, 4096)
            vertBuffer = device.makeBuffer(length: vertCapacity * 20,
                                           options: [.storageModeShared])
        }
        if indexCapacity < indices {
            indexCapacity = max(indices * 2, 8192)
            indexBuffer = device.makeBuffer(length: indexCapacity * 2,
                                            options: [.storageModeShared])
        }
    }

    /// Debug (SHITTIM_TEX_DUMP): write a render target's contents to a PNG
    /// so the baked flash textures can be inspected directly. getBytes is
    /// not legal on .private storage, so blit into a shared copy first
    /// (this buffer waits behind everything already committed on the queue).
    private func dumpTexture(_ tex: MTLTexture, _ path: String) {
        let w = tex.width, h = tex.height
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: tex.pixelFormat,
                                                            width: w, height: h, mipmapped: false)
        desc.storageMode = .shared
        desc.usage = [.shaderRead, .renderTarget]
        guard let cmd = queue.makeCommandBuffer(),
              let shared = device.makeTexture(descriptor: desc),
              let blit = cmd.makeBlitCommandEncoder() else { return }
        blit.copy(from: tex, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: w, height: h, depth: 1),
                  to: shared, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        cmd.commit()
        cmd.waitUntilCompleted()
        var px = [UInt8](repeating: 0, count: w * h * 4)
        shared.getBytes(&px, bytesPerRow: w * 4,
                        from: MTLRegion(origin: MTLOrigin(x: 0, y: 0, z: 0),
                                        size: MTLSize(width: w, height: h, depth: 1)),
                        mipmapLevel: 0)
        for i in stride(from: 0, to: px.count, by: 4) { px.swapAt(i, i + 2) }  // bgra -> rgba
        let cs = CGColorSpaceCreateDeviceRGB()
        guard let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
              let img = ctx.makeImage() else { return }
        let rep = NSBitmapImageRep(cgImage: img)
        if let png = rep.representation(using: .png, properties: [:]) {
            try? png.write(to: URL(fileURLWithPath: path))
            say("dumped \(path)")
        }
    }
}
