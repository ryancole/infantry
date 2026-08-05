#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// What a player may call themselves, and the only place any of it is decided.
//
// A name is the one piece of a match that a person authors, which means it is
// the one piece that arrives from outside: typed into a settings screen on a
// machine we don't own, saved in a file anybody may edit, and then sent up a
// wire by a client that could have been rewritten between the two. So the rule
// lives here rather than at any of those three points, and all three run it —
// the menu as the player types, the file on the way in, and the server on
// whatever the packet claims. A name that has been through Clean is a name
// every one of them agrees about; a name that hasn't is somebody's guess.
//
// What it allows is deliberately narrow. Uppercase, because the game is drawn
// in it and a lowercase name would be the one lowercase word on the screen.
// Letters, digits, a space, a dash and an underscore, because that is enough
// to be somebody and short of enough to draw a box around a squadmate's row.
// Twelve characters, because the two places a name is read — a scoreboard
// column that also has to fit the class beside it, and a label under a soldier
// that must not be wider than the ground they're standing on — both stop being
// readable somewhere around there. The limit is what the readouts can hold,
// which is the honest place for a limit to come from.
namespace PlayerName
{
    inline constexpr size_t kMaxLength = 12;

    inline char Upper(char c)
    {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }

    inline bool Allowed(char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
               c == '_';
    }

    // Whatever was offered, as the name it's allowed to be: uppercased,
    // stripped of anything not on the list, with runs of spaces collapsed and
    // the ends trimmed, cut to length. Never fails and never rejects — a name
    // is not a password, and the answer to a bad one is the good one nearest
    // it rather than a door closed on somebody trying to join a game. An offer
    // with nothing usable in it comes back empty, which the server reads as
    // "hasn't said" (see Unnamed).
    inline std::string Clean(std::string_view raw)
    {
        std::string out;
        for (const char c : raw)
        {
            const char up = Upper(c);
            if (!Allowed(up))
                continue;
            if (up == ' ' && (out.empty() || out.back() == ' '))
                continue;
            if (out.size() >= kMaxLength)
                break;
            out.push_back(up);
        }
        // The cut above can land mid-gap, and a trailing space is a name with
        // an invisible character on the end of it.
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    // What somebody who hasn't said gets called, off the id the server issued
    // them. It's the one place that id is shown to anybody, and it's there
    // because two silent players have to end up on two different rows — a
    // board with two soldiers called SOLDIER on it is a board that can't be
    // read, and the number that tells them apart is one the server already
    // holds.
    inline std::string Unnamed(int player)
    {
        return Clean("SOLDIER " + std::to_string(player));
    }

    // What the soldiers who can't choose are called.
    //
    // A bot used to be its class, and that was honest as far as it went: an AI
    // place really does get dealt a fresh class every life, so MARINE was a true
    // statement about what was standing there. What it wasn't was a person. Five
    // rows of class names on a side is a parts list, and an arena where only the
    // people have anything written under them is an arena that reads as one
    // player among furniture. Naming them costs nothing the fight can feel — a
    // bot is exactly as dangerous as it was — and it buys the thing a scoreboard
    // is for: the soldier who has killed you three times is somebody, and now
    // you know which one they are.
    //
    // Handles rather than surnames, because that is what the roster of a 1999
    // online shooter looked like and because a bot here is standing in for a
    // player rather than for a character. Every one of them survives Clean
    // unchanged, which is not a coincidence to rely on — they go through it like
    // everything else does.
    inline constexpr const char* kBotNames[] = {
        "REAPER",  "VIPER",   "HAVOC",    "GHOST",   "RAZOR",   "TALON",
        "WIDOW",   "COBRA",   "FALCON",   "JACKAL",  "MAVERICK", "RECON",
        "SABLE",   "SHRIKE",  "SPECTRE",  "VULTURE", "WRAITH",  "ANVIL",
        "BADGER",  "BISHOP",  "CINDER",   "DAGGER",  "DIESEL",  "DRIFTER",
        "FLINT",   "HAMMER",  "IRONSIDE", "KESTREL", "LOCKJAW", "NOMAD",
        "ONYX",    "QUARRY",  "RIVET",    "SALVO",   "SHADOW",  "SLATE",
        "STITCH",  "TREAD",   "VECTOR",   "WARDEN",
    };

    // Four times the ten a match fields, so a side is never dealt a name it
    // has to share and a server that has been running long enough to hand out
    // every one of them has had a lot of people come and go.
    inline constexpr size_t kBotNameCount = sizeof(kBotNames) / sizeof(kBotNames[0]);
}
