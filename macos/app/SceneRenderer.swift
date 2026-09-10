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

    /// Render text into a texture (straight alpha), for the in-scene clock.
    func makeTextTexture(text: String, fontSize: CGFloat) -> MTLTexture? {
        let W = 720, H = 200
        let img = NSImage(size: NSSize(width: W, height: H))
        img.lockFocus()
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: fontSize, weight: .light),
            .foregroundColor: NSColor.white,
        ]
        (text as NSString).draw(at: NSPoint(x: 10, y: 10), withAttributes: attrs)
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

        guard let cmd = queue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }

        var screen = SIMD2<Float>(Float(w), Float(h))
        enc.setVertexBytes(&screen, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
        var hole = holeRect
        enc.setFragmentBytes(&hole, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
        let smp = device.makeSamplerState(descriptor: {
            let d = MTLSamplerDescriptor()
            d.minFilter = .linear
            d.magFilter = .linear
            d.sAddressMode = .clampToEdge
            d.tAddressMode = .clampToEdge
            return d
        }())
        enc.setFragmentSamplerState(smp, index: 0)

        enc.setVertexBuffer(vertBuffer, offset: 0, index: 0)

        for i in 0..<Int(batchCount) {
            let b = sb[i]
            let page = Int(b.page)
            let blend = Int(b.blend)
            let indexOffset = Int(b.indexOffset)
            let indexCount = Int(b.indexCount)
            guard page < Int(sb_page_count()), let tex = texture(forPage: page) else { continue }
            enc.setRenderPipelineState(pipelines[min(max(blend, 0), 3)])
            enc.setFragmentTexture(tex, index: 0)
            enc.drawIndexedPrimitives(type: .triangle, indexCount: indexCount,
                                      indexType: .uint16, indexBuffer: indexBuffer!,
                                      indexBufferOffset: indexOffset * 2)
        }

        // in-scene clock, top-centre
        if let clockTexture, clockRect.width > 0 {
            enc.setRenderPipelineState(pipelines[0])
            enc.setFragmentTexture(clockTexture, index: 0)
            struct QuadVert { var x, y, u, v: Float; var r, g, b, a: UInt8 }
            let cw = Float(clockRect.width), ch = Float(clockRect.height)
            let x0 = Float(clockRect.minX), y0 = Float(clockRect.minY)
            let quad: [QuadVert] = [
                QuadVert(x: x0,      y: y0,      u: 0, v: 0, r: 255, g: 255, b: 255, a: 255),
                QuadVert(x: x0 + cw, y: y0,      u: 1, v: 0, r: 255, g: 255, b: 255, a: 255),
                QuadVert(x: x0,      y: y0 + ch, u: 0, v: 1, r: 255, g: 255, b: 255, a: 255),
                QuadVert(x: x0 + cw, y: y0 + ch, u: 1, v: 1, r: 255, g: 255, b: 255, a: 255),
            ]
            enc.setVertexBytes(quad, length: 4 * 20, index: 0)
            // the quad uses the same vertex layout (pos, uv, color, stride 20)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
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
}
