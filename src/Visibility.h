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
    // Everything is clipped to the arena square, which doubles as the far
    // boundary: outside the arena nothing is ever visible.
    std::vector<DirectX::XMFLOAT2> ComputePolygon(const DirectX::XMFLOAT2& viewer,
                                                  const std::vector<Rect>& occluders,
                                                  float arenaHalf);

    // True if the open segment viewer->point crosses no occluder. Used to
    // cull entities (projectiles, later enemies) the player cannot see.
    bool IsPointVisible(const DirectX::XMFLOAT2& viewer, const DirectX::XMFLOAT2& point,
                        const std::vector<Rect>& occluders);
}
