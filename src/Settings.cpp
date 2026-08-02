#include "Settings.h"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

std::string Settings::FilePath()
{
    const char* local = std::getenv("LOCALAPPDATA");
    const std::filesystem::path dir =
        local ? std::filesystem::path(local) / "Infantry" : std::filesystem::path(".");
    return (dir / "settings.toml").string();
}

bool Settings::Load()
{
    toml::table root;
    try
    {
        root = toml::parse_file(FilePath());
    }
    catch (const toml::parse_error&)
    {
        return false; // no file, or one that's been edited into nonsense
    }

    if (const std::optional<bool> value = root["audio"]["background"].value<bool>())
        backgroundAudio = *value;
    return true;
}

bool Settings::Save() const
{
    const std::filesystem::path path = FilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    toml::table audio;
    audio.insert("background", backgroundAudio);

    toml::table root;
    root.insert("audio", std::move(audio));

    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return false;
    file << "# Infantry settings. Written by the game; safe to edit by hand.\n"
         << "# Anything this file doesn't mention keeps the game's default.\n"
         << "#\n"
         << "# audio.background - keep playing sound while the game window is\n"
         << "#                    behind something else.\n\n"
         << root << '\n';
    return static_cast<bool>(file);
}
