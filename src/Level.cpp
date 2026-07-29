#include "Level.h"

#include <nlohmann/json.hpp>

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
            level.spawns.push_back({ js.value("team", 0), ToFloat3(js.at("pos")) });

        if (level.arenaHalf <= 0.0f)
            throw std::runtime_error(path + ": bounds.halfExtent must be positive");
        if (level.spawns.empty())
            throw std::runtime_error(path + ": at least one spawn is required");
        return level;
    }
    catch (const json::exception& e)
    {
        throw std::runtime_error(path + ": " + e.what());
    }
}
