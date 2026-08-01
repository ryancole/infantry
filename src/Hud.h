#pragma once

#include "Renderer.h"

#include <DirectXMath.h>

// The heads-up display: one bottom-centered cluster of icon + value modules,
// drawn in screen space over the finished frame.
//
// It reads nothing but the snapshot below, which the game fills once a frame.
// That keeps the layout free of gameplay entanglement — the numbers arrive
// already decided, so the only thing this file is responsible for is how they
// look — and means a second consumer (a spectator view, a replay) can put the
// same readout on screen without owning a Game.
namespace Hud
{
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
        int npcs;
        DirectX::XMFLOAT4 accent; // class color, for the panel's edge light
        // False during the respawn wait: the loadout on show isn't in anyone's
        // hands, so the whole cluster fades rather than lying about a magazine
        // the player can't fire.
        bool alive;
    };

    void Render(Renderer& renderer, const State& state);
}
