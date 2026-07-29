#include "Level.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

using nlohmann::json;

namespace
{
    DirectX::XMFLOAT3 ToFloat3(const json& j)
    {
        if (!j.is_array() || j.size() != 3)
            throw json::type_error::create(302, "expected an array of 3 numbers", &j);
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    }
}

LevelData LevelData::Load(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("level file not found: " + path);

    try
    {
        const json j = json::parse(file);

        LevelData level;
        level.name = j.value("name", std::string{});
        level.arenaHalf = j.at("bounds").at("halfExtent").get<float>();

        for (const json& jo : j.value("objects", json::array()))
        {
            Object obj;
            obj.model = jo.value("model", std::string{});
            obj.pos = ToFloat3(jo.at("pos"));
            obj.scale = jo.value("scale", 1.0f);
            obj.yaw = jo.value("yaw", 0.0f);
            if (jo.contains("collider"))
                obj.collider = ToFloat3(jo.at("collider").at("size"));
            if (obj.model.empty() && !obj.collider)
                throw std::runtime_error(path + ": object needs a model, a collider, or both");
            level.objects.push_back(std::move(obj));
        }

        for (const json& js : j.value("spawns", json::array()))
            level.spawns.push_back({ js.at("team").get<int>(), ToFloat3(js.at("pos")) });

        if (level.arenaHalf <= 0.0f)
            throw std::runtime_error(path + ": bounds.halfExtent must be positive");
        if (level.spawns.empty())
            throw std::runtime_error(path + ": at least one spawn is required");

        // Normalize to spawns[team]: exactly one spawn per team, ids 0..N-1.
        std::sort(level.spawns.begin(), level.spawns.end(),
                  [](const Spawn& a, const Spawn& b) { return a.team < b.team; });
        for (size_t i = 0; i < level.spawns.size(); ++i)
        {
            const Spawn& s = level.spawns[i];
            if (s.team != static_cast<int>(i))
                throw std::runtime_error(path + ": spawn teams must be 0.." +
                                         std::to_string(level.spawns.size() - 1) +
                                         " with one spawn each (got team " +
                                         std::to_string(s.team) + ")");
            if (std::abs(s.pos.x) > level.arenaHalf || std::abs(s.pos.z) > level.arenaHalf)
                throw std::runtime_error(path + ": team " + std::to_string(s.team) +
                                         " spawn is outside the arena bounds");
        }
        return level;
    }
    catch (const json::exception& e)
    {
        throw std::runtime_error(path + ": " + e.what());
    }
}
