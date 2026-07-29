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
    };

    struct Spawn
    {
        int team;
        DirectX::XMFLOAT3 pos;
    };

    std::string name;
    float arenaHalf = 32.0f; // arena spans [-arenaHalf, arenaHalf] on x and z
    std::vector<Object> objects;
    // One spawn per team. Load enforces team ids 0..N-1 with no gaps or
    // duplicates and sorts by team, so spawns.size() is the level's team count
    // and spawns[team] is that team's spawn point.
    std::vector<Spawn> spawns;

    // Throws std::runtime_error (with the offending path in the message) on a
    // missing file, malformed JSON, or schema mismatch.
    static LevelData Load(const std::string& path);
};
