#pragma once

#include "gpu.h"
#include "../../render/src/scene.h"

#include <spine/spine.h>
#include <spine/SkeletonRenderer.h>

#include <unordered_map>
#include <vector>

namespace gpu {

struct PageTextures {
    std::unordered_map<const sl::Image *, Texture> map;

    ID3D11ShaderResourceView *Get(ID3D11Device *dev, const sl::Image *img) {
        if (!img) return nullptr;
        auto it = map.find(img);
        if (it != map.end()) return it->second.srv.Get();
        Texture t;
        if (!t.Create(dev, img->rgba.data(), img->width, img->height)) return nullptr;
        auto res = map.emplace(img, std::move(t));
        return res.first->second.srv.Get();
    }
};

struct Batch {
    ID3D11ShaderResourceView *srv = nullptr;
    Blend blend = Blend::Normal;
    UINT indexCount = 0, startIndex = 0, baseVertex = 0;
};

inline Blend toBlend(spine::BlendMode m) {
    switch (m) {
    case spine::BlendMode_Additive: return Blend::Additive;
    case spine::BlendMode_Multiply: return Blend::Multiply;
    case spine::BlendMode_Screen:   return Blend::Screen;
    default:                        return Blend::Normal;
    }
}

inline size_t AppendSkeleton(ID3D11Device *dev, PageTextures &pages,
                             spine::Skeleton &sk, spine::SkeletonRenderer &renderer,
                             const scene::Viewport &vp, int W, int H,
                             std::vector<Vertex> &verts, std::vector<uint16_t> &indices,
                             std::vector<Batch> &batches) {
    const float sx = W / vp.width;
    const float sy = H / vp.height;
    const float top = vp.bottom + vp.height;
    size_t triangles = 0;

    for (spine::RenderCommand *cmd = renderer.render(sk); cmd; cmd = cmd->next) {
        const sl::Image *tex = static_cast<const sl::Image *>(cmd->texture);
        ID3D11ShaderResourceView *srv = pages.Get(dev, tex);
        if (!srv) continue;

        Batch b;
        b.srv = srv;
        b.blend = toBlend(cmd->blendMode);
        b.baseVertex = (UINT)verts.size();
        b.startIndex = (UINT)indices.size();
        b.indexCount = (UINT)cmd->numIndices;

        verts.reserve(verts.size() + cmd->numVertices);
        for (int i = 0; i < cmd->numVertices; i++) {
            Vertex v;

            v.x = (cmd->positions[i * 2] - vp.left) * sx;
            v.y = (top - cmd->positions[i * 2 + 1]) * sy;
            v.u = cmd->uvs[i * 2];
            v.v = cmd->uvs[i * 2 + 1];
            const uint32_t c = cmd->colors[i];
            v.a = ((c >> 24) & 0xFF) / 255.0f;
            v.r = ((c >> 16) & 0xFF) / 255.0f;
            v.g = ((c >>  8) & 0xFF) / 255.0f;
            v.b = ( c        & 0xFF) / 255.0f;
            verts.push_back(v);
        }
        indices.insert(indices.end(), cmd->indices, cmd->indices + cmd->numIndices);
        batches.push_back(b);
        triangles += size_t(cmd->numIndices) / 3;
    }
    return triangles;
}

struct SlotMute {
    spine::Skeleton &sk;
    std::vector<float> saved;

    SlotMute(spine::Skeleton &skeleton, scene::Pass pass) : sk(skeleton) {
        const scene::Classification c = scene::classify(sk);
        Init([&](size_t i) { return scene::hiddenInPass(c, i, pass); });
    }

    SlotMute(spine::Skeleton &skeleton, int lo, int hi) : sk(skeleton) {
        Init([&](size_t i) { return (int)i < lo || (int)i > hi; });
    }

    struct OccluderTag {};
    SlotMute(spine::Skeleton &skeleton, int after, OccluderTag) : sk(skeleton) {
        const scene::Classification c = scene::classify(sk);
        Init([&](size_t i) { return (int)i <= after || c.isChar[i]; });
    }

    template <class Pred> void Init(Pred hide) {
        saved.resize(sk.getSlots().size());
        for (size_t i = 0; i < sk.getSlots().size(); i++) {
            spine::Slot *s = sk.getSlots()[i];
            saved[i] = s->getColor().a;
            if (hide(i)) s->getColor().a = 0.0f;
        }
    }
    ~SlotMute() {
        for (size_t i = 0; i < sk.getSlots().size(); i++)
            sk.getSlots()[i]->getColor().a = saved[i];
    }
};

}
