#pragma once

#include "PlayerClass.h"
#include "Renderer.h"

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

    // Green -> amber -> red for a 0..1 share of health, thresholds a hit or two
    // apart at typical damage. Exposed because the panel is no longer the only
    // place health gets drawn: a medic sees their squadmates' health over their
    // heads, and a soldier at a third has to look the same there as the player
    // at a third does down here. One rule, read from one place.
    DirectX::XMFLOAT4 HealthColor(float fraction);
}
