#pragma once

#include <DirectXMath.h>
#include <optional>
#include <string>
#include <vector>

// A level as authored on disk (assets/levels/*.json): arena bounds, placed
// objects, and spawn points. Pure data — the Game decides how to turn it into
// geometry, physics bodies, and models.
struct LevelData
{
    // One placed thing. Visuals and collision are independent: an object with
    // only a collider is invisible blockout geometry (drawn as a debug cube),
    // one with only a model is decoration, one with both is a solid prop.
    struct Object
    {
        std::string model; // path relative to the exe dir; empty = no visual
        DirectX::XMFLOAT3 pos; // ground anchor point
        float scale;
        float yaw;
        // Full extents of an axis-aligned collision box standing on pos
        // (centered on x/z, rising from pos.y). Authored at scale 1; the
        // game multiplies it by `scale` so it tracks the model.
        std::optional<DirectX::XMFLOAT3> collider;
        // Whether this collider also blocks line of sight. Unset — the usual
        // case — means it's decided by height: anything standing at eye level
        // hides what's behind it, and anything under it can be seen over.
        //
        // It's an override rather than a rule because height is the wrong
        // question for thin things. A tree trunk is chest high and stops a
        // round that happens to hit it, but nobody hides behind one: treating
        // every trunk in a jungle as a sight-blocker would fill the fog with
        // flickering slivers, and cost the visibility sweep — which is
        // quadratic in occluders — far more than the trees are worth.
        std::optional<bool> blocksSight;
    };

    struct Spawn
    {
        int team;
        DirectX::XMFLOAT3 pos;
    };

    std::string name;
    // Arena half-extents: it spans [-x, x] and [-z, z] about the origin. Two
    // numbers rather than one because a map is not obliged to be square — a
    // fight between two bases with something in the middle to go around wants
    // to be long in the direction the bases face and no deeper than the going
    // around is worth. `"halfExtent": 32` still means a 64-unit square.
    DirectX::XMFLOAT2 arenaHalf = { 32.0f, 32.0f };
    std::vector<Object> objects;
    // One spawn per team. Load enforces team ids 0..N-1 with no gaps or
    // duplicates and sorts by team, so spawns.size() is the level's team count
    // and spawns[team] is that team's spawn point.
    std::vector<Spawn> spawns;

    // Throws std::runtime_error (with the offending path in the message) on a
    // missing file, malformed JSON, or schema mismatch.
    static LevelData Load(const std::string& path);
};
