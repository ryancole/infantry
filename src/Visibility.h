#pragma once

#include <DirectXMath.h>
#include <vector>

// 2D line-of-sight on the ground plane. Occluders are axis-aligned rectangles
// (the footprints of sight-blocking level colliders); the viewer is a point.
// Height is ignored here — the Game decides which colliders are tall enough
// to block sight before handing their footprints over.
namespace Visibility
{
    struct Rect
    {
        float minX, minZ, maxX, maxZ;
    };

    // The visibility polygon around `viewer`, as points sorted by angle.
    // Rays are cast at every occluder corner (plus the arena corners), so
    // consecutive points always span a straight run of the same edge — the
    // region between viewer and the polygon is exactly what can be seen.
    // Everything is clipped to the arena, which doubles as the far boundary:
    // outside it nothing is ever visible. `arenaHalf` is the half-extent on x
    // and z — the arena is a rectangle, not necessarily a square.
    std::vector<DirectX::XMFLOAT2> ComputePolygon(const DirectX::XMFLOAT2& viewer,
                                                  const std::vector<Rect>& occluders,
                                                  const DirectX::XMFLOAT2& arenaHalf);

    // True if the open segment viewer->point crosses no occluder. Used to
    // cull entities (projectiles, later enemies) the player cannot see.
    bool IsPointVisible(const DirectX::XMFLOAT2& viewer, const DirectX::XMFLOAT2& point,
                        const std::vector<Rect>& occluders);

    // The same test from several eyes at once: true if any of them has a clear
    // line to the point. What a side sees is what its soldiers see between
    // them, so one clear line out of five is the whole answer and the rest of
    // the list goes untested.
    bool IsPointVisibleAny(const std::vector<DirectX::XMFLOAT2>& eyes,
                           const DirectX::XMFLOAT2& point, const std::vector<Rect>& occluders);
}
