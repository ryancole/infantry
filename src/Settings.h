#pragma once

#include <string>

// The player's preferences that aren't controls. Bindings answers "what is
// this key wired to"; this answers everything else the options screens set,
// and it exists for the same reason: a preference that lives as a constant
// next to the code that reads it is a preference the player can't have.
//
// It's a plain struct with a file attached rather than a class with getters,
// because there's nothing to guard — every field is a value the player picked
// and the game reads. It round-trips through a TOML file next to the bindings,
// so both halves of "how I like it" survive the build directory going away and
// both can be hand-edited by somebody who'd rather do that.
//
// Nothing here is applied at load time. Whoever owns the thing a setting
// describes reads it when it matters — see Game::Update and the ear.
struct Settings
{
    // Whether the game keeps making noise while its window is behind something
    // else. Off by default, which is what a player alt-tabbing away to read
    // something almost always means, and what most games do without asking.
    // On is for the second monitor: the fight stays audible while the hands
    // are elsewhere.
    bool backgroundAudio = false;

    // What the other players call you: the name on your row of the scoreboard,
    // sent once when you join a server and cleaned at both ends
    // (PlayerName::Clean). It is a setting rather than something asked for at
    // the door because it is the same answer every time — a player types it
    // once and then never thinks about it again, which is precisely the kind of
    // thing a file is for and a prompt isn't.
    //
    // The default is the Windows account name, on the same reasoning Discovery
    // uses the machine name to label a server on the join screen: the machine
    // already knows what its owner is called, and a first run that puts a real
    // name on the board is better than one that makes the player go and find a
    // menu. Empty is legal and means "hasn't said" — the server names you off
    // the id it issued you instead, so a row is always somebody.
    std::string playerName = DefaultName();

    // %LOCALAPPDATA%\Infantry\settings.toml — beside bindings.toml, and for
    // the same reasons.
    static std::string FilePath();
    // The account name this machine's owner signed in under, as a name the
    // game will accept, or empty if it can't be had. Only ever the starting
    // value of the field above — once the file exists, the file is the answer.
    static std::string DefaultName();
    // False when there's nothing to read or it's unusable, in which case the
    // defaults stand. A setting the file doesn't mention keeps its default, so
    // a partial file is legal.
    bool Load();
    bool Save() const;
};
