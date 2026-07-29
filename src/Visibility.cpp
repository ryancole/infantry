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

    void AppendRectSegments(std::vector<Segment>& out, const Visibility::Rect& r)
    {
        out.push_back({ { r.minX, r.minZ }, { r.maxX, r.minZ } });
        out.push_back({ { r.maxX, r.minZ }, { r.maxX, r.maxZ } });
        out.push_back({ { r.maxX, r.maxZ }, { r.minX, r.maxZ } });
        out.push_back({ { r.minX, r.maxZ }, { r.minX, r.minZ } });
    }

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
                                         const std::vector<Rect>& occluders, float arenaHalf)
    {
        std::vector<Segment> segments;
        segments.reserve(occluders.size() * 4 + 4);
        for (const Rect& r : occluders)
            AppendRectSegments(segments, r);
        AppendRectSegments(segments, { -arenaHalf, -arenaHalf, arenaHalf, arenaHalf });

        // Three rays per unique endpoint: dead-on plus a nudge to either side,
        // so corners produce both the near hit (on the edge) and the far hit
        // (past the corner).
        std::vector<float> angles;
        angles.reserve(segments.size() * 6);
        for (const Segment& s : segments)
        {
            for (const XMFLOAT2& e : { s.a, s.b })
            {
                const float a = std::atan2(e.y - viewer.y, e.x - viewer.x);
                angles.push_back(a - kAngleEpsilon);
                angles.push_back(a);
                angles.push_back(a + kAngleEpsilon);
            }
        }
        std::sort(angles.begin(), angles.end());

        std::vector<XMFLOAT2> poly;
        poly.reserve(angles.size());
        for (float a : angles)
        {
            const XMFLOAT2 dir = { std::cos(a), std::sin(a) };
            float nearest = std::numeric_limits<float>::infinity();
            for (const Segment& s : segments)
                nearest = std::min(nearest, RayHit(viewer, dir, s));
            if (!std::isfinite(nearest)) // viewer outside the arena; skip
                continue;
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
}
