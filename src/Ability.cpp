#include "Ability.h"

#include <algorithm>
#include <cmath>

// The rules half of an ability — what it does, when it starts, what ends it.
// This file runs wherever the simulation runs, a headless server included;
// the marks an ability draws and what it lets its owner see live next door in
// AbilityDraw.cpp, on the machines that have a screen to put them on.

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    // --- Heal ---

    // Of everyone standing in the cone, the one the user is most nearly pointed
    // at — not the nearest, which is what the blade picks. The two rules differ
    // because the two cones do: a swing reaches barely past the body in front of
    // it, so distance is the only thing that separates two targets inside it,
    // while a dressing reaches across a room, where a soldier three units off to
    // the side and one directly ahead are plainly not the same choice. Pointing
    // is what "looking at" means at this range.
    //
    // No sight test here: the list arrived already filtered to what the user can
    // see. Treating someone through a wall would be the only thing in the game
    // that could, and it's the caller that knows where the walls are.
    const Ability::Target* HealTarget(const Ability::Def& def, const Ability::Scene& scene)
    {
        if (!scene.allies || def.heal.reach <= 0.0f)
            return nullptr;

        const float cosArc = std::cos(def.heal.arc);
        const Ability::Target* best = nullptr;
        float bestDot = 0.0f;
        for (const Ability::Target& ally : *scene.allies)
        {
            Vector3 toward = ally.pos - scene.user.pos;
            toward.y = 0.0f;
            const float dist = toward.Length();
            if (dist > def.heal.reach || dist < 1e-4f)
                continue;
            const float dot = toward.Dot(scene.aimDir) / dist;
            if (dot < cosArc)
                continue;
            if (best && dot <= bestDot)
                continue;
            best = &ally;
            bestDot = dot;
        }
        return best;
    }

    // Whether starting would accomplish anything. Spending a fourteen-second
    // cooldown on health nobody is missing is never what the key meant, so it
    // does nothing at all rather than something worthless — the same courtesy a
    // reload does for a full magazine. "Nobody" has to include whoever the user
    // is pointed at: an unhurt medic standing over a wounded squadmate is the
    // exact situation the ability exists for, and testing their own health alone
    // would refuse it.
    bool HealWanted(const Ability::Def& def, const Ability::Scene& scene)
    {
        if (*scene.user.hp < scene.maxHp)
            return true;
        const Ability::Target* mate = HealTarget(def, scene);
        return mate && *mate->hp < scene.maxHp;
    }

    // `step` seconds of healing. Paid out by the second, not banked until the
    // end: a dressing that only settled up on completion would be worth exactly
    // nothing in the moment it's most likely to be interrupted, which is the
    // moment it's most likely to be needed.
    //
    // The friendly gets the same share the user does rather than half of it, so
    // treating somebody is strictly worth more than treating nobody and the
    // reach is a thing to work for. Who that is gets asked again every frame:
    // the healing follows the aim, so a medic can sweep it across two wounded
    // soldiers and genuinely split it between them, and one who turns away has
    // stopped treating whoever they turned away from.
    void HealStep(const Ability::Def& def, Ability::Runtime& rt, const Ability::Scene& scene,
                  float step)
    {
        const float given = def.heal.amount * step / def.duration;
        *scene.user.hp = std::min(scene.maxHp, *scene.user.hp + given);
        if (const Ability::Target* mate = HealTarget(def, scene))
        {
            *mate->hp = std::min(scene.maxHp, *mate->hp + given);
            rt.reachedPos = mate->pos;
            rt.reached = true;
        }
    }
}

bool Ability::Ready(const Def& def, const Runtime& rt)
{
    return def.kind != Kind::None && rt.time <= 0.0f && rt.cooldown <= 0.0f;
}

void Ability::Drop(Runtime& rt)
{
    rt.time = 0.0f;
    rt.reached = false;
}

bool Ability::Update(const Def& def, Runtime& rt, const Scene& scene, float dt, bool requested,
                     bool weaponUsed)
{
    rt.cooldown = std::max(0.0f, rt.cooldown - dt);
    if (def.kind == Kind::None)
        return false;

    // Ending a run, whichever way it happened. Nothing else in the loadout
    // stops for an ability — the medic dressing a wound can still run, still
    // turn, still be shot at — so what it costs is the weapon: firing, throwing
    // or swinging drops the dressing where it stands. A second press is the
    // same deal, and it's how a player who changes their mind gets their hands
    // back. Since the cooldown is measured from here rather than from the
    // start, being interrupted costs the whole ability rather than the part
    // that was left, which is what makes deciding to see it out a decision.
    if (rt.time > 0.0f && (requested || weaponUsed))
    {
        rt.time = 0.0f;
        rt.reached = false;
        rt.cooldown = def.cooldown;
    }

    if (rt.time > 0.0f)
    {
        const float step = std::min(dt, rt.time);
        rt.time -= step;
        rt.reached = false;
        switch (def.kind)
        {
        case Kind::None: break;
        case Kind::Heal: HealStep(def, rt, scene, step); break;
        }
        if (rt.time <= 0.0f)
        {
            rt.reached = false;
            rt.cooldown = def.cooldown;
        }
        return false;
    }

    // Starting one. The weapon test is here and not only above so that a press
    // arriving on the same frame as a shot is a press that never started
    // anything, rather than one that started something and immediately paid the
    // full cooldown for losing it — a player leaning on the trigger would
    // otherwise burn fourteen seconds on a dressing that never got a frame to
    // itself, with nothing on screen long enough to explain why.
    if (!requested || weaponUsed || !Ready(def, rt))
        return false;
    bool wanted = false;
    switch (def.kind)
    {
    case Kind::None: break;
    case Kind::Heal: wanted = HealWanted(def, scene); break;
    }
    if (!wanted)
        return false;
    rt.time = def.duration;
    return true;
}
