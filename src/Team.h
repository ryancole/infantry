#pragma once

#include <DirectXMath.h>
#include <cstddef>

// The sides, and what they wear. Which side a soldier is on is the first thing
// a player has to be able to read about them — before what class they are,
// before what they're holding — because it's the only one that decides whether
// to shoot. So it gets the loudest channel the model has: the armor itself.
//
// Blue and red, and deliberately nothing else. They're the two hues nothing in
// the arena competes with (the floor grid is slate, the props are grey, the
// tracers are yellow), they're far enough apart to survive the fog quads dimming
// them, and they're the pair the HUD's roster rows already use — so the count in
// the corner and the bodies on the field say the same thing in the same colors.
//
// The table is indexed by the team ids the level's spawn points carry. A level
// with more sides than this would need more entries; GetTeamDef wraps rather
// than reading off the end, so an unexpected id draws somebody else's color
// instead of garbage.
struct TeamDef
{
    const char* name;
    DirectX::XMFLOAT4 color; // armor plate; the class marker rides on top of it
};

inline constexpr size_t kTeamCount = 2;

inline constexpr TeamDef kTeamDefs[kTeamCount] = {
    // Saturated rather than pastel, and mid-brightness rather than light: the
    // plate is drawn at full value and again at 55% for the dark trim, and both
    // ends of that have to stay recognizably the same color.
    { "BLUE", { 0.20f, 0.42f, 0.88f, 1.0f } },
    { "RED",  { 0.86f, 0.22f, 0.18f, 1.0f } },
};

inline const TeamDef& GetTeamDef(int team)
{
    return kTeamDefs[static_cast<size_t>(team) % kTeamCount];
}

inline const DirectX::XMFLOAT4& TeamColor(int team)
{
    return GetTeamDef(team).color;
}
