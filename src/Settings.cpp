#include "Settings.h"

#include "PlayerName.h"

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

std::string Settings::DefaultName()
{
    // The environment variable rather than GetUserNameA, so this file stays
    // the plain std-library thing it is — it already reads LOCALAPPDATA the
    // same way, and a settings file is not worth a windows.h.
    const char* user = std::getenv("USERNAME");
    return user ? PlayerName::Clean(user) : std::string();
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
    // Cleaned on the way in, because this is a file the player is invited to
    // edit and what they type into it is an offer, not a name. A line of
    // punctuation cleans to nothing, which is the same as not having said —
    // the server will call them something.
    if (const std::optional<std::string> value = root["player"]["name"].value<std::string>())
        playerName = PlayerName::Clean(*value);
    return true;
}

bool Settings::Save() const
{
    const std::filesystem::path path = FilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    toml::table audio;
    audio.insert("background", backgroundAudio);

    toml::table player;
    player.insert("name", playerName);

    toml::table root;
    root.insert("audio", std::move(audio));
    root.insert("player", std::move(player));

    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return false;
    file << "# Infantry settings. Written by the game; safe to edit by hand.\n"
         << "# Anything this file doesn't mention keeps the game's default.\n"
         << "#\n"
         << "# audio.background - keep playing sound while the game window is\n"
         << "#                    behind something else.\n"
         << "# player.name      - what the other players see on the scoreboard.\n"
         << "#                    Up to " << PlayerName::kMaxLength
         << " of A-Z, 0-9, space, dash and\n"
         << "#                    underscore; anything else is dropped, and an\n"
         << "#                    empty name means the server picks one.\n\n"
         << root << '\n';
    return static_cast<bool>(file);
}
