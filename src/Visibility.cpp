#include "Visibility.h"

#include <algorithm>
#include <cmath>
#include <limits>

using DirectX::XMFLOAT2;

namespace
{
    struct Segment
    {
        XMFLOAT2 a, b;
    };

    // Nudge applied on each side of a corner ray so the sweep sees both the
    // edge that ends at the corner and whatever lies beyond it.
    constexpr float kAngleEpsilon = 1e-4f;

    // Rays spent on the far edge of sight, where the range cuts it rather than
    // a wall. A hundred and twenty-eight leaves the arc's widest bow under a
    // hundredth of a unit at the ranges this game sees — a fraction of a pixel
    // — and costs a fraction of the rays the corners already ask for.
    constexpr int kArcRays = 128;

    // Distance along the ray (origin p, unit direction d) to the segment, or
    // infinity if they miss.
    float RayHit(const XMFLOAT2& p, const XMFLOAT2& d, const Segment& s)
    {
        const float sx = s.b.x - s.a.x;
        const float sz = s.b.y - s.a.y;
        const float denom = d.x * sz - d.y * sx;
        if (std::abs(denom) < 1e-9f)
            return std::numeric_limits<float>::infinity();

        const float px = s.a.x - p.x;
        const float pz = s.a.y - p.y;
        const float t = (px * sz - pz * sx) / denom; // along the ray
        const float u = (px * d.y - pz * d.x) / denom; // along the segment
        if (t < 0.0f || u < 0.0f || u > 1.0f)
            return std::numeric_limits<float>::infinity();
        return t;
    }
}

namespace Visibility
{
    std::vector<XMFLOAT2> ComputePolygon(const XMFLOAT2& viewer,
                                         const std::vector<Rect>& occluders,
                                         const XMFLOAT2& arenaHalf, float range)
    {
        const Rect bounds = { -arenaHalf.x, -arenaHalf.y, arenaHalf.x, arenaHalf.y };

        // Three rays per corner: dead-on plus a nudge to either side, so corners
        // produce both the near hit (on the edge) and the far hit (past the
        // corner). Cast off the rectangles rather than off the segments —
        // every corner belongs to two edges, and casting per edge endpoint
        // doubled the sweep for a second set of identical answers.
        std::vector<float> angles;
        angles.reserve((occluders.size() + 1) * 12 + kArcRays);
        // The ring the range makes, cast whether or not anything is out there:
        // in the open the corner rays are far apart or absent, and the far edge
        // of sight has to be round on its own account. Fine enough that the
        // chords between them read as an arc at any zoom the camera offers.
        for (int i = 0; i < kArcRays; ++i)
            angles.push_back(-3.14159265f + 6.28318531f * (static_cast<float>(i) / kArcRays));
        const auto corners = [&](const Rect& r) {
            for (const XMFLOAT2& e : { XMFLOAT2{ r.minX, r.minZ }, XMFLOAT2{ r.maxX, r.minZ },
                                       XMFLOAT2{ r.maxX, r.maxZ }, XMFLOAT2{ r.minX, r.maxZ } })
            {
                const float a = std::atan2(e.y - viewer.y, e.x - viewer.x);
                angles.push_back(a - kAngleEpsilon);
                angles.push_back(a);
                angles.push_back(a + kAngleEpsilon);
            }
        };
        for (const Rect& r : occluders)
            corners(r);
        corners(bounds);
        std::sort(angles.begin(), angles.end());

        // Sort the walls into angular bins around the viewer, so a ray only
        // gets tested against the ones it could possibly hit.
        //
        // Every ray against every edge is the honest version of this sweep and
        // it is quadratic in occluders — fine for a blockout arena with a dozen
        // walls, hopeless on a real map, where the count is in the hundreds and
        // the ray count grows with it. A rectangle seen from a point covers one
        // arc and can only be hit from inside it, and that is a fact this can be
        // cheap about: bin by arc, then a ray reads its own bin. Nothing is
        // approximated — every edge a ray could reach is still tested, so the
        // polygon is the same one the exhaustive version produced.
        constexpr int kBins = 512;
        constexpr float kTau = 6.28318531f;
        // Kept between calls: this runs once a frame, and rebuilding five
        // hundred vectors each time costs more than the sweep it saves.
        thread_local std::vector<std::vector<Segment>> bins(kBins);
        for (std::vector<Segment>& bin : bins)
            bin.clear();

        const auto binOf = [](float a) {
            int i = static_cast<int>((a + 3.14159265f) * (kBins / kTau));
            return std::clamp(i, 0, kBins - 1);
        };

        const auto fileRect = [&](const Rect& r) {
            const Segment edges[4] = {
                { { r.minX, r.minZ }, { r.maxX, r.minZ } },
                { { r.maxX, r.minZ }, { r.maxX, r.maxZ } },
                { { r.maxX, r.maxZ }, { r.minX, r.maxZ } },
                { { r.minX, r.maxZ }, { r.minX, r.minZ } },
            };

            // Standing inside it, the arena included: it covers every direction.
            const bool inside = viewer.x >= r.minX && viewer.x <= r.maxX &&
                                viewer.y >= r.minZ && viewer.y <= r.maxZ;
            int lo = 0, hi = kBins - 1;
            if (!inside)
            {
                // The arc a rectangle covers is everything except its widest
                // gap — which is the one place a ray can pass without meeting
                // it. Found on the corner angles, sorted.
                float ca[4] = {
                    std::atan2(r.minZ - viewer.y, r.minX - viewer.x),
                    std::atan2(r.minZ - viewer.y, r.maxX - viewer.x),
                    std::atan2(r.maxZ - viewer.y, r.maxX - viewer.x),
                    std::atan2(r.maxZ - viewer.y, r.minX - viewer.x),
                };
                std::sort(std::begin(ca), std::end(ca));
                int gap = 3; // the wrap from the last corner back to the first
                float widest = ca[0] + kTau - ca[3];
                for (int i = 0; i < 3; ++i)
                    if (ca[i + 1] - ca[i] > widest)
                    {
                        widest = ca[i + 1] - ca[i];
                        gap = i;
                    }
                // The span runs from the far side of the gap round to its near
                // side, so it may cross the seam at pi; a margin of one bin
                // covers the epsilon-nudged rays cast at the corners.
                lo = binOf(ca[(gap + 1) % 4]) - 1;
                hi = binOf(ca[gap]) + 1;
                if (gap != 3)
                    hi += kBins; // wrapped: walk past the seam and wrap on write
            }
            for (int i = lo; i <= hi; ++i)
            {
                std::vector<Segment>& bin = bins[((i % kBins) + kBins) % kBins];
                bin.insert(bin.end(), edges, edges + 4);
            }
        };

        for (const Rect& r : occluders)
            fileRect(r);
        fileRect(bounds);

        std::vector<XMFLOAT2> poly;
        poly.reserve(angles.size());
        for (float a : angles)
        {
            const XMFLOAT2 dir = { std::cos(a), std::sin(a) };
            float nearest = std::numeric_limits<float>::infinity();
            for (const Segment& s : bins[binOf(a)])
                nearest = std::min(nearest, RayHit(viewer, dir, s));
            if (!std::isfinite(nearest)) // viewer outside the arena; skip
                continue;
            // Tested against the walls first and cut to the range after: what
            // the eye reaches is whichever comes sooner.
            nearest = std::min(nearest, range);
            poly.push_back({ viewer.x + dir.x * nearest, viewer.y + dir.y * nearest });
        }
        return poly;
    }

    bool IsPointVisible(const XMFLOAT2& viewer, const XMFLOAT2& point,
                        const std::vector<Rect>& occluders)
    {
        const float dx = point.x - viewer.x;
        const float dz = point.y - viewer.y;
        for (const Rect& r : occluders)
        {
            // Slab-clip the segment (t in [0,1]) against the rectangle.
            float t0 = 0.0f, t1 = 1.0f;
            bool miss = false;
            const float d[2] = { dx, dz };
            const float lo[2] = { r.minX - viewer.x, r.minZ - viewer.y };
            const float hi[2] = { r.maxX - viewer.x, r.maxZ - viewer.y };
            for (int axis = 0; axis < 2 && !miss; ++axis)
            {
                if (std::abs(d[axis]) < 1e-9f)
                {
                    if (lo[axis] > 0.0f || hi[axis] < 0.0f)
                        miss = true;
                    continue;
                }
                float tn = lo[axis] / d[axis];
                float tf = hi[axis] / d[axis];
                if (tn > tf)
                    std::swap(tn, tf);
                t0 = std::max(t0, tn);
                t1 = std::min(t1, tf);
                if (t0 > t1)
                    miss = true;
            }
            if (!miss)
                return false;
        }
        return true;
    }

    bool IsPointVisibleAny(const std::vector<Eye>& eyes, const XMFLOAT2& point,
                           const std::vector<Rect>& occluders)
    {
        for (const Eye& eye : eyes)
        {
            const float dx = point.x - eye.pos.x;
            const float dz = point.y - eye.pos.y;
            if (dx * dx + dz * dz <= eye.range * eye.range &&
                IsPointVisible(eye.pos, point, occluders))
                return true;
        }
        return false;
    }
}
