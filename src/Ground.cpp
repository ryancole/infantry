#include "Ground.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace DirectX;

namespace
{
    // --- The turf sheet.
    //
    // A grid of vertices with a color on each and the triangles between them
    // interpolating, rather than a tiling of flat-colored squares: at the
    // camera's default zoom one world unit is about fifty pixels, so tiles of
    // any size worth building would read as a checkerboard — which is the grid
    // again, in different clothes. Two units between vertices is fine enough
    // that the mottling has no visible period and coarse enough that the whole
    // arena is a few thousand triangles.
    constexpr float kCellTarget = 2.0f;
    // Cells to a chunk, on each axis. Eight units square is a little wider
    // than a soldier's stride and a lot smaller than the viewport, which is
    // the balance the cull wants: fine enough to reject most of the map,
    // coarse enough that the test itself isn't the cost.
    constexpr int kChunkCells = 4;
    // The band of bare earth the turf wears down to at the arena's edge. It's
    // what's left of the old border line, and it does that line's whole job —
    // saying where the world stops — without drawing anything: the ground runs
    // out, the way ground does.
    constexpr float kRimWidth = 2.5f;

    // Jungle floor, in the range the old grid was drawn in so nothing else on
    // the field has to be re-tuned around it. Dark on purpose: the arena is
    // read by the things standing on it, and a bright floor is a floor that
    // competes with the soldiers for the eye.
    constexpr XMFLOAT4 kTurfDark = { 0.032f, 0.068f, 0.038f, 1.0f };
    constexpr XMFLOAT4 kTurfLight = { 0.145f, 0.245f, 0.115f, 1.0f };
    constexpr XMFLOAT4 kTurfEarth = { 0.165f, 0.135f, 0.080f, 1.0f };
    constexpr XMFLOAT4 kRimEarth = { 0.230f, 0.165f, 0.090f, 1.0f };

    // Noise scales, in world units. The broad one makes patches of light and
    // shade a soldier crosses in a couple of seconds; the fine one keeps any
    // one patch from being a flat wash; the third decides where the turf has
    // worn through to dirt, and is scaled so bare ground is occasional.
    constexpr float kPatchScale = 11.0f;
    constexpr float kSpeckScale = 2.7f;
    constexpr float kWearScale = 17.0f;
    constexpr float kWearThreshold = 0.62f;

    // --- The grass.
    //
    // Tufts of a few blades each, scattered rather than planted: real ground
    // cover is clumpy, and an even spread of single blades reads as a texture
    // where a clumpy one reads as a place.
    // Density is what decides whether this reads as ground cover or as weeds,
    // and it's the number the whole thing costs: at three blades a tuft the
    // arena carries about a quarter of a million vertices, a few megabytes,
    // which is worth it once and would not be worth it twice.
    constexpr float kTuftsPerUnit = 1.5f; // per square unit of ground
    constexpr int kBladesPerTuft = 3;
    constexpr float kTuftSpread = 0.13f; // how far a blade sits off its tuft's middle
    // Ankle height on a soldier who stands about 1.7 units. Grass has to be
    // seen to be worth drawing and must never be worth hiding behind, and
    // that's the whole range that satisfies both.
    constexpr float kBladeMinHeight = 0.22f;
    constexpr float kBladeMaxHeight = 0.42f;
    constexpr float kBladeWidth = 0.062f;
    constexpr float kBladeBend = 0.15f; // how far the tip leans off the root
    // Grass keeps this far clear of anything solid, so a blade at the foot of
    // a trench wall doesn't stick out through the far side of it.
    constexpr float kSolidMargin = 0.10f;

    // A blade's tip, between the two ends of what one can catch. There is no
    // lighting on this geometry — the batch vertex carries a color and nothing
    // else — so the light is baked in by which way a blade leans, which is
    // enough to make a field of them look lit rather than painted.
    // The shaded end is kept at about the turf's own brightness rather than
    // under it: a blade darker than the ground it stands in reads as something
    // dropped there, and a field of those reads as litter.
    constexpr XMFLOAT4 kBladeShade = { 0.090f, 0.160f, 0.078f, 1.0f };
    constexpr XMFLOAT4 kBladeLit = { 0.255f, 0.400f, 0.150f, 1.0f };
    // Where that light comes from, on the ground plane. It's the camera's own
    // starting yaw (45°), so at the angle the game opens at the lit faces are
    // the ones turned toward the player.
    constexpr XMFLOAT2 kLightAz = { 0.7071f, 0.7071f };
    // The blade's root takes the turf's color rather than a constant, so grass
    // grows out of the ground it's standing on instead of being laid over it.
    // Slightly darker than the turf, which is what the base of anything is.
    constexpr float kRootShade = 0.8f;

    uint32_t Hash(uint32_t a, uint32_t b, uint32_t salt)
    {
        uint32_t h = a * 0x8da6b343u + b * 0xd8163841u + salt * 0xcb1ab31fu;
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        h *= 0x297a2d39u;
        h ^= h >> 15;
        return h;
    }

    float Unit(uint32_t h) { return static_cast<float>(h & 0xffffffu) / 16777215.0f; }

    // Smoothed value noise on a lattice of `cell` units. Keyed on the lattice
    // coordinate rather than on anything the caller holds, so two chunks that
    // meet along an edge compute the same color for the vertices they share
    // and the seam between them doesn't exist.
    float Noise(float x, float z, float cell, uint32_t salt)
    {
        const float fx = x / cell;
        const float fz = z / cell;
        const float bx = std::floor(fx);
        const float bz = std::floor(fz);
        const auto ix = static_cast<uint32_t>(static_cast<int32_t>(bx));
        const auto iz = static_cast<uint32_t>(static_cast<int32_t>(bz));

        float tx = fx - bx;
        float tz = fz - bz;
        tx = tx * tx * (3.0f - 2.0f * tx);
        tz = tz * tz * (3.0f - 2.0f * tz);

        const float a = Unit(Hash(ix, iz, salt));
        const float b = Unit(Hash(ix + 1, iz, salt));
        const float c = Unit(Hash(ix, iz + 1, salt));
        const float d = Unit(Hash(ix + 1, iz + 1, salt));
        const float lo = a + (b - a) * tx;
        const float hi = c + (d - c) * tx;
        return lo + (hi - lo) * tz;
    }

    // A run of numbers for one tuft: which corner of its chunk it stands in,
    // how tall it is, which way each of its blades leans. Seeded off the
    // chunk and the tuft's index, so the field is the same field every time
    // the map is loaded and on every machine that loads it.
    struct Scatter
    {
        uint32_t state;

        float operator()()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return Unit(state);
        }

        float operator()(float lo, float hi) { return lo + (hi - lo) * (*this)(); }
    };

    XMFLOAT4 Mix(const XMFLOAT4& a, const XMFLOAT4& b, float t)
    {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f };
    }

    XMFLOAT4 TurfColor(float x, float z, const XMFLOAT2& arenaHalf)
    {
        const float patch = Noise(x, z, kPatchScale, 0x01u);
        const float speck = Noise(x, z, kSpeckScale, 0x02u);
        const float wear = Noise(x, z, kWearScale, 0x03u);

        // The broad term is stretched about its middle before the fine one is
        // added: two uniform values summed pile up around a half and come out
        // as a flat wash, which is the one thing this is here not to be.
        const float t = (patch - 0.5f) * 1.7f + (speck - 0.5f) * 0.5f + 0.5f;
        XMFLOAT4 c = Mix(kTurfDark, kTurfLight, std::clamp(t, 0.0f, 1.0f));
        c = Mix(c, kTurfEarth, std::clamp((wear - kWearThreshold) * 3.0f, 0.0f, 1.0f));

        // The rim, squared so the turf holds its color most of the way out and
        // then gives up quickly rather than fading across the last three units.
        const float edge = std::min(arenaHalf.x - std::abs(x), arenaHalf.y - std::abs(z));
        const float rim = 1.0f - std::clamp(edge / kRimWidth, 0.0f, 1.0f);
        return Mix(c, kRimEarth, rim * rim);
    }

    bool InSolid(const std::vector<Visibility::Rect>& solids, float x, float z)
    {
        for (const Visibility::Rect& r : solids)
            if (x >= r.minX - kSolidMargin && x <= r.maxX + kSolidMargin &&
                z >= r.minZ - kSolidMargin && z <= r.maxZ + kSolidMargin)
                return true;
        return false;
    }

    // Culls the field against the viewport and hands what survives to the
    // renderer in as few batches as the vertex cap allows. `grass` picks the
    // layer and with it the pass: turf is ordinary opaque geometry, blades are
    // drawn only where the fog's mark says the player's side can see.
    void DrawChunks(Renderer& renderer, const Ground::Field& field, std::vector<Vertex>& scratch,
                    bool grass)
    {
        const XMMATRIX identity = XMMatrixIdentity();
        const auto flush = [&] {
            if (scratch.empty())
                return;
            const auto count = static_cast<uint32_t>(scratch.size());
            if (grass)
                renderer.DrawTrianglesSeen(scratch.data(), count, identity);
            else
                renderer.DrawTriangles(scratch.data(), count, identity);
            scratch.clear();
        };

        scratch.clear();
        for (const Ground::Chunk& chunk : field)
        {
            const std::vector<Vertex>& src = grass ? chunk.blades : chunk.turf;
            if (src.empty() || !renderer.IsSphereVisible(chunk.center, chunk.radius))
                continue;
            if (scratch.size() + src.size() > Renderer::kBatchVertices)
                flush();
            scratch.insert(scratch.end(), src.begin(), src.end());
        }
        flush();
    }
}

Ground::Field Ground::Build(const XMFLOAT2& arenaHalf, const std::vector<Visibility::Rect>& solids)
{
    Field field;

    // The cell size is derived rather than fixed, so the sheet lands exactly
    // on the arena's edges: an arena is whatever the level said it was, and a
    // floor that stopped short of it or hung off it would be a floor with a
    // seam of void along one side.
    const int nx = std::max(1, static_cast<int>(std::lround(2.0f * arenaHalf.x / kCellTarget)));
    const int nz = std::max(1, static_cast<int>(std::lround(2.0f * arenaHalf.y / kCellTarget)));
    const float cellX = 2.0f * arenaHalf.x / static_cast<float>(nx);
    const float cellZ = 2.0f * arenaHalf.y / static_cast<float>(nz);

    const int chunksX = (nx + kChunkCells - 1) / kChunkCells;
    const int chunksZ = (nz + kChunkCells - 1) / kChunkCells;
    field.reserve(static_cast<size_t>(chunksX) * static_cast<size_t>(chunksZ));

    std::vector<Visibility::Rect> nearby; // solids overlapping the chunk in hand

    for (int cz = 0; cz < chunksZ; ++cz)
    {
        for (int cx = 0; cx < chunksX; ++cx)
        {
            // The last chunk on each axis is clipped rather than padded, which
            // is why the cell counts are carried around instead of a size.
            const int i0 = cx * kChunkCells;
            const int i1 = std::min(nx, i0 + kChunkCells);
            const int j0 = cz * kChunkCells;
            const int j1 = std::min(nz, j0 + kChunkCells);
            const float x0 = -arenaHalf.x + static_cast<float>(i0) * cellX;
            const float x1 = -arenaHalf.x + static_cast<float>(i1) * cellX;
            const float z0 = -arenaHalf.y + static_cast<float>(j0) * cellZ;
            const float z1 = -arenaHalf.y + static_cast<float>(j1) * cellZ;

            Chunk chunk;
            chunk.center = { (x0 + x1) * 0.5f, kBladeMaxHeight * 0.5f, (z0 + z1) * 0.5f };
            chunk.radius = 0.5f * std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0)) +
                           kBladeMaxHeight;

            chunk.turf.reserve(static_cast<size_t>(i1 - i0) * static_cast<size_t>(j1 - j0) * 6);
            for (int j = j0; j < j1; ++j)
            {
                for (int i = i0; i < i1; ++i)
                {
                    const float ax = -arenaHalf.x + static_cast<float>(i) * cellX;
                    const float az = -arenaHalf.y + static_cast<float>(j) * cellZ;
                    const float bx = ax + cellX;
                    const float bz = az + cellZ;
                    const Vertex v00 = { XMFLOAT3{ ax, 0.0f, az }, TurfColor(ax, az, arenaHalf) };
                    const Vertex v10 = { XMFLOAT3{ bx, 0.0f, az }, TurfColor(bx, az, arenaHalf) };
                    const Vertex v11 = { XMFLOAT3{ bx, 0.0f, bz }, TurfColor(bx, bz, arenaHalf) };
                    const Vertex v01 = { XMFLOAT3{ ax, 0.0f, bz }, TurfColor(ax, bz, arenaHalf) };
                    chunk.turf.push_back(v00);
                    chunk.turf.push_back(v10);
                    chunk.turf.push_back(v11);
                    chunk.turf.push_back(v00);
                    chunk.turf.push_back(v11);
                    chunk.turf.push_back(v01);
                }
            }

            // Every solid on the map against every tuft would be a few million
            // rectangle tests; every solid against every chunk, once, is a few
            // hundred thousand, and what's left over for the tufts is the
            // handful of walls actually standing in this square of ground.
            nearby.clear();
            for (const Visibility::Rect& r : solids)
                if (r.maxX >= x0 - kSolidMargin && r.minX <= x1 + kSolidMargin &&
                    r.maxZ >= z0 - kSolidMargin && r.minZ <= z1 + kSolidMargin)
                    nearby.push_back(r);

            const int tufts = static_cast<int>((x1 - x0) * (z1 - z0) * kTuftsPerUnit);
            chunk.blades.reserve(static_cast<size_t>(tufts) * kBladesPerTuft * 3);
            for (int t = 0; t < tufts; ++t)
            {
                // Seeded so that a tuft's numbers depend on nothing but where
                // it is in the field. The or-one is the xorshift's one rule:
                // a state of zero stays zero forever.
                Scatter dice{ Hash(static_cast<uint32_t>(cx) * 0x0001f000u +
                                       static_cast<uint32_t>(t),
                                   static_cast<uint32_t>(cz), 0x51ed2701u) |
                              1u };

                const float px = dice(x0, x1);
                const float pz = dice(z0, z1);
                if (InSolid(nearby, px, pz))
                    continue;

                const float height = dice(kBladeMinHeight, kBladeMaxHeight);
                const float lean = dice(0.0f, XM_2PI);
                const XMFLOAT4 turf = TurfColor(px, pz, arenaHalf);
                const XMFLOAT4 root = { turf.x * kRootShade, turf.y * kRootShade,
                                        turf.z * kRootShade, 1.0f };

                for (int b = 0; b < kBladesPerTuft; ++b)
                {
                    // Blades fan out around the tuft rather than lying at
                    // independent angles, so a clump reads as one plant.
                    const float yaw = lean +
                                      static_cast<float>(b) * (XM_2PI / kBladesPerTuft) +
                                      dice(-0.4f, 0.4f);
                    const float cs = std::cos(yaw);
                    const float sn = std::sin(yaw);
                    const float off = dice(0.0f, kTuftSpread);
                    const float rx = px + cs * off;
                    const float rz = pz + sn * off;
                    const float h = height * dice(0.7f, 1.0f);
                    const float bend = kBladeBend * dice(0.5f, 1.3f);
                    const float hw = kBladeWidth * 0.5f;

                    const float lit = 0.5f + 0.5f * (cs * kLightAz.x + sn * kLightAz.y);
                    const XMFLOAT4 tip = Mix(kBladeShade, kBladeLit, lit);

                    // One triangle: a root the blade's width across, laid
                    // square to the lean, and a tip out along it. Drawn with
                    // culling off, so which way round the two roots go is
                    // nobody's business.
                    chunk.blades.push_back({ XMFLOAT3{ rx - sn * hw, 0.0f, rz + cs * hw }, root });
                    chunk.blades.push_back({ XMFLOAT3{ rx + sn * hw, 0.0f, rz - cs * hw }, root });
                    chunk.blades.push_back({ XMFLOAT3{ rx + cs * bend, h, rz + sn * bend }, tip });
                }
            }

            field.push_back(std::move(chunk));
        }
    }

    return field;
}

void Ground::DrawTurf(Renderer& renderer, const Field& field, std::vector<Vertex>& scratch)
{
    DrawChunks(renderer, field, scratch, false);
}

void Ground::DrawGrass(Renderer& renderer, const Field& field, std::vector<Vertex>& scratch)
{
    DrawChunks(renderer, field, scratch, true);
}
