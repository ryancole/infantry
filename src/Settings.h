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

    // %LOCALAPPDATA%\Infantry\settings.toml — beside bindings.toml, and for
    // the same reasons.
    static std::string FilePath();
    // False when there's nothing to read or it's unusable, in which case the
    // defaults stand. A setting the file doesn't mention keeps its default, so
    // a partial file is legal.
    bool Load();
    bool Save() const;
};
