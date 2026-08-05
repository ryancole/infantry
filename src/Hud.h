#pragma once

#include "PlayerClass.h"
#include "Renderer.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <cstddef>

// The heads-up display: a bottom-centered cluster of icon + value modules for
// what this soldier is carrying, and a roster in the top-right corner for how
// the two sides are doing. Both drawn in screen space over the finished frame.
//
// They're apart because they answer different questions and are read at
// different moments. The loadout is about you and is glanced at mid-fight; the
// roster is about the match and is read between them — including, especially,
// while dead. Putting the strength of both squads in the same row as the
// magazine would have made the row wider without making either easier to find.
//
// It reads nothing but the snapshot below, which the game fills once a frame.
// That keeps the layout free of gameplay entanglement — the numbers arrive
// already decided, so the only thing this file is responsible for is how they
// look — and means a second consumer (a spectator view, a replay) can put the
// same readout on screen without owning a Game. The class tables it leans on
// are the same kind of thing: pure data with no simulation behind it, and the
// place a class's own facts (its color, its ability) are already written down.
namespace Hud
{
    // One key cap and what it does. The key text arrives as a string rather
    // than being written down here because the player owns their layout now:
    // what RELOAD is spelled with is a fact about their bindings, and this file
    // has no business holding a second opinion about it. What the key *does* is
    // still stated here — that's the name of a thing, not a binding.
    struct Hint
    {
        const char* key;
        const char* label;
    };

    struct State
    {
        float hp;
        float maxHp;
        int ammo;
        int magazine;         // rounds a full magazine holds; 0 = not magazine-fed
        float reloadFraction; // 0..1 progress through a reload; < 0 when not reloading
        int melee;            // melee swings left
        int meleeCharges;     // swings a full recovery gives back
        // 0..1 progress through the melee recovery, and < 0 whenever there's a
        // swing left to make — the charges also come back from a partial spend,
        // but that isn't a wait, so it isn't reported as one. Kept separate
        // from reloadFraction because the two run independently: the blade is
        // what a soldier has left mid-reload, so both readouts can be counting
        // at once and neither can stand in for the other.
        float meleeRecoverFraction;
        int grenades;
        // The class ability, or null for a class that hasn't got one — which
        // leaves the module out of the cluster entirely rather than drawing an
        // empty slot, since an ability isn't kit that can be spent. The
        // definition comes over whole because the HUD needs more than a number
        // off it: the name goes on the key hint, and the cooldown is what turns
        // the seconds below into a bar.
        const Ability::Def* ability;
        float abilityFraction; // 0..1 through a use; < 0 when it isn't running
        float abilityCooldown; // seconds until it's ready again; 0 = now
        // The roster, for the corner panel. Both counts include every soldier
        // of that side standing — the player among their own — because the two
        // rows are read against each other, and a friendly count that left the
        // player out would say "outnumbered" every time it said "even". A side
        // is short exactly when somebody on it is waiting to respawn, and that
        // is the thing the panel is for.
        int allies;
        int enemies;
        int teamSize; // slots a side fields; how many pips a row draws
        // What the match is being decided on: how long it has left, and what
        // each side has killed. The clock is seconds remaining and reads as
        // M:SS; `matchOver` says it has run out, which is when the score
        // stops being a running total and starts being the result. They sit
        // in the roster panel rather than anywhere else because "how are we
        // doing" and "how long have I got to fix it" are one glance.
        float clock;
        bool matchOver;
        int allyScore;
        int enemyScore;
        // What the two sides are wearing out on the field. Handed over rather
        // than chosen here so the corner can't drift from the arena: the row
        // marked as yours is your armor's color, whichever side the player ends
        // up on. The HUD lightens them for a small icon on a dark panel, but it
        // never picks them.
        DirectX::XMFLOAT4 allyColor;
        DirectX::XMFLOAT4 enemyColor;

        // The key-cap row above the panel, in draw order. It arrives whole
        // rather than being assembled here because half of it is now the
        // player's bindings and the other half — which of them are worth the
        // space, and what the ability on this class is called — is the game's;
        // neither is a fact this file can look up.
        const Hint* hints;
        size_t hintCount;
        DirectX::XMFLOAT4 accent; // class color, for the panel's edge light
        // False during the respawn wait: the loadout on show isn't in anyone's
        // hands, so the whole cluster fades rather than lying about a magazine
        // the player can't fire.
        bool alive;
    };

    void Render(Renderer& renderer, const State& state);

    // --- The scoreboard, held up over the arena while the player asks for it.
    //
    // The corner panel says how the two sides are doing; this says how everyone
    // in them is doing, which is a different question and gets a different
    // amount of screen. It's the only readout here that covers soldiers the
    // player can't see — the fog hides bodies, not the board — so it's also the
    // only one that has to arrive from the simulation rather than from what's
    // on screen.

    // Who a row belongs to. Mirrors World::Slot::Held rather than including it:
    // this file has never heard of the simulation, and what it needs off that
    // enum is three ways to draw a row, not a shared type.
    enum class Holder
    {
        Ai,
        Human,
        Left, // a player who has gone; their score stays on the board
    };

    // One row: a soldier's place on a side for the whole match, and what
    // they've made of it.
    struct ScoreRow
    {
        int team;
        // Who is standing in the slot: a player's own name, or — for a place
        // the AI is fielding — the class, which is the honest name for it,
        // since the class really is re-dealt every time the slot refills. Null
        // for a place nothing has stood in yet. This file doesn't know which
        // kind it's been handed and doesn't need to: a row is drawn with the
        // name it has.
        const char* name;
        // The class beside it, for a row whose name is a person's. Null when
        // the name *is* the class, which is what stops an AI row saying MARINE
        // twice. It's here because a scoreboard that only named the people
        // would have quietly stopped answering "which of them is the sniper",
        // and on a board of ten that is half of what the enemy column is read
        // for.
        const char* cls;
        Holder holder;
        bool you;
        int kills;
        int deaths;
    };

    struct Scoreboard
    {
        // Every slot on the field, in whatever order they arrive — including
        // the ones belonging to players who have left, which is what makes a
        // column add up to the total over it. The panel splits them by side and
        // sorts each column itself, because how a scoreboard is ordered is a
        // fact about scoreboards.
        const ScoreRow* rows;
        size_t rowCount;
        // The two sides, in the order their columns are drawn: the player's
        // first. Names and colors come in rather than being looked up, the same
        // as the corner panel's, so the board can't disagree with the arena
        // about which side is which.
        const char* teamNames[2];
        DirectX::XMFLOAT4 teamColors[2];
        int teams[2]; // team ids, to match rows against
        int teamScores[2];
        float clock;
        bool matchOver;
    };

    void RenderScoreboard(Renderer& renderer, const Scoreboard& board);

    // --- The radar, in the bottom-left corner: the whole arena, small.
    //
    // The camera shows about twenty-six units of a map that is nearly two
    // hundred across, so most of a match happens off screen. The corner panel
    // says how many of the enemy are still standing and the fog says whether
    // any of them can be seen; what neither says is *where* — and on this
    // ground "which way across" is the question the whole map is built around.
    // That is the one thing the radar adds, and it's why it draws the terrain
    // as well as the bodies: a field of dots with no shape behind it would say
    // where everyone is without saying what they're behind.
    //
    // It is aligned to the *camera*, not to the arena. That is the one thing
    // about this panel that isn't a matter of taste. The view is isometric and
    // its yaw starts at 45°, which means world +x travels left and down the
    // screen rather than right — so a radar laid out on the arena's own axes
    // doesn't merely sit at an angle to what the player sees, it puts their
    // base on the wrong side of the panel. Left and right disagreeing is the
    // one error a radar cannot survive; being turned 45° from the map's axes
    // is a cost worth paying to fix it, and the map is a shape the player
    // learns off this panel anyway rather than one they brought with them.
    //
    // So the arena arrives as a diamond, and the panel is the box that holds
    // it — which is also why the box is near enough square while the map it
    // draws is a long letterbox.
    //
    // The blips arrive already filtered. What may be drawn is a question about
    // the fog, and the fog is the game's business: this file is handed a list
    // and draws all of it.

    // One soldier on the panel, at a spot on the ground. Positions are world
    // coordinates — x and z, the arena's own axes — because where the panel
    // puts them is a layout decision and layout decisions belong down here.
    struct Blip
    {
        float x, z;
        // The same two colors the body is wearing in the arena: the side's
        // armor, and the class mark riding on top of it. Handed over rather
        // than looked up, for the reason every other color here is — the radar
        // and the field have to agree about who is who, and a second opinion
        // is a second thing to keep in step.
        DirectX::XMFLOAT4 team;
        DirectX::XMFLOAT4 cls;
        // The player's own soldier, drawn with a heading out of it. A dot for
        // the player would only repeat what the camera already centers on;
        // which way they're pointed is the part the arena doesn't say at a
        // glance, and it's what makes the other blips' bearings mean something.
        bool you;
        float aimX, aimZ; // unit facing, read only when `you`
    };

    struct Radar
    {
        // Half-extents on x and z: the arena spans [-x, x] by [-z, z]. The
        // panel is sized from these once they've been turned to face the
        // camera, and one scale serves both axes — a map squashed to fill a
        // fixed box would put every bearing on it slightly wrong.
        DirectX::XMFLOAT2 arenaHalf;
        // Which way is right on screen, as a direction on the ground: the
        // arena's x and z components of the camera's own right vector. This is
        // what turns the map to face the player, and taking it rather than a
        // yaw is deliberate — the camera already hands this vector out for
        // standing health bars square to the view, so the panel and the arena
        // are turned by the same number and cannot drift apart. Screen up is
        // derived from it rather than passed: on the ground plane the two are
        // always a quarter turn apart, and a caller that could set them
        // separately is a caller that could set them inconsistently.
        DirectX::XMFLOAT2 screenRight;
        // The sight-blockers, as the footprints the fog is cut against. Taken
        // as Visibility::Rect rather than mirrored into a local type the way
        // Holder mirrors World::Slot::Held: that enum is the simulation's, and
        // this is a rectangle. Borrowed for the call, so 125 of them cost
        // nothing to hand over. Axis-aligned in the arena, and so aligned to
        // nothing once they've been turned to face the camera — each is drawn
        // as a quad of four corners rather than as a rectangle.
        const Visibility::Rect* walls;
        size_t wallCount;
        const Blip* blips;
        size_t blipCount;
        // How much of the arena the box holds. 1 is the whole of it, fitted;
        // above that the map is drawn larger than the box and the box becomes a
        // window onto it, which is what stops a letterbox map from leaving the
        // corners of a square panel empty. The panel itself never changes size —
        // zooming changes what is in the box, not how much screen the box is
        // worth.
        float zoom;
        // Where the window looks when there is more map than box: the player's
        // own soldier, or the spot they died on while they wait. Clamped to the
        // arena in here rather than by the caller, because how much of the map
        // the box currently holds is a fact about the layout and the layout is
        // decided down here — a window that ran off the edge would scroll the
        // ground away from under a soldier standing still.
        float focusX, focusZ;
        // The controls that move the zoom, as the same key caps the loadout
        // draws, sat over the panel. A zoom nobody knows about is a zoom that
        // stays where it started, and this is the only readout on screen whose
        // controls are neither under a finger already nor on the row beneath
        // the loadout — so it says so itself, next to the thing they act on,
        // which is the only place the answer is any use.
        //
        // They arrive as caps rather than as two key names for the same reason
        // State::hints does: what a control is spelled is the player's
        // business, and this file has no way to look it up.
        const Hint* hints;
        size_t hintCount;
    };

    void RenderRadar(Renderer& renderer, const Radar& radar);

    // Green -> amber -> red for a 0..1 share of health, thresholds a hit or two
    // apart at typical damage. Exposed because the panel is no longer the only
    // place health gets drawn: a medic sees their squadmates' health over their
    // heads, and a soldier at a third has to look the same there as the player
    // at a third does down here. One rule, read from one place.
    DirectX::XMFLOAT4 HealthColor(float fraction);
}
