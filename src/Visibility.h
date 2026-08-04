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

    // One place a side is looking from, and how far it reaches. The range
    // travels with the position because it belongs to the soldier standing
    // there rather than to the game: a squad is a list of unlike eyes now,
    // not one radius applied to several points (see ClassDef::sight). Pairing
    // them here is what stops the two from being passed separately and drifting
    // — a sniper's eye can't be handed to the fog with a marine's reach.
    struct Eye
    {
        DirectX::XMFLOAT2 pos;
        float range;
    };

    // The visibility polygon around `viewer`, as points sorted by angle.
    // Rays are cast at every occluder corner (plus the arena corners), so
    // consecutive points always span a straight run of the same edge — the
    // region between viewer and the polygon is exactly what can be seen.
    // Everything is clipped to the arena, which doubles as the far boundary:
    // outside it nothing is ever visible. `arenaHalf` is the half-extent on x
    // and z — the arena is a rectangle, not necessarily a square.
    //
    // `range` is how far the eye reaches: no ray is allowed past it, so open
    // ground ends in an arc rather than at a wall. The arc is why the sweep
    // casts a ring of evenly spaced rays as well as the ones the corners ask
    // for — without them a polygon in the open would be whatever few-sided
    // shape the corners happened to make of a circle.
    std::vector<DirectX::XMFLOAT2> ComputePolygon(const DirectX::XMFLOAT2& viewer,
                                                  const std::vector<Rect>& occluders,
                                                  const DirectX::XMFLOAT2& arenaHalf,
                                                  float range);

    // True if the open segment viewer->point crosses no occluder. Used to
    // cull entities (projectiles, later enemies) the player cannot see.
    bool IsPointVisible(const DirectX::XMFLOAT2& viewer, const DirectX::XMFLOAT2& point,
                        const std::vector<Rect>& occluders);

    // The same test from several eyes at once, each reaching its own distance:
    // true if any has both the range and a clear line. What a side sees is
    // what its soldiers see between them, so one eye out of five is the whole
    // answer and the rest of the list goes untested. The distance is checked
    // first, which is also what makes a list of eyes cheap — most of them are
    // nowhere near the point being asked about, and now that the ranges differ
    // it's the near, short-sighted ones that get rejected on arithmetic while
    // the sniper's line is the one actually traced.
    bool IsPointVisibleAny(const std::vector<Eye>& eyes, const DirectX::XMFLOAT2& point,
                           const std::vector<Rect>& occluders);
}
