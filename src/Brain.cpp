#include "Brain.h"

using namespace DirectX::SimpleMath;

namespace
{
    float Rand(std::mt19937& rng, float lo, float hi)
    {
        return std::uniform_real_distribution<float>(lo, hi)(rng);
    }

    // --- Rifleman ---
    //
    // Engage range is deliberately shorter than the body's sight range and
    // shorter than the player can see, so a rifleman can be picked at from a
    // distance before it has any idea. It's a trait rather than a fact about
    // the world: a marksman's would be longer, and the body would hand it the
    // same contacts either way.
    constexpr float kEngageRange = 22.0f;
    // Closer than this it stops advancing and starts circling. The whole shape
    // of the behavior is in these two numbers — walk in until here, then orbit
    // — which is why a second brain is a second pair of numbers and a different
    // idea rather than a flag on this one.
    constexpr float kPreferredRange = 11.0f;
    constexpr float kCombatSpeed = 0.85f; // fraction of class move speed while fighting
    constexpr float kWanderSpeed = 0.5f;  // and while it has nothing to fight

    // Nearest of everyone in range. Nearest is the only rule a rifleman has:
    // it has no reason to prefer one target over another, and picking the
    // closest is what keeps it from walking past someone shooting it.
    const Brain::Contact* NearestEnemy(const Brain::Senses& senses)
    {
        if (!senses.enemies)
            return nullptr;
        const Brain::Contact* best = nullptr;
        for (const Brain::Contact& c : *senses.enemies)
        {
            if (c.dist > kEngageRange || c.dist < 1e-3f)
                continue;
            if (!best || c.dist < best->dist)
                best = &c;
        }
        return best;
    }

    Brain::Intent ThinkRifleman(Brain::Memory& mem, const Brain::Senses& senses, float dt,
                                std::mt19937& rng)
    {
        Brain::Intent intent;

        if (const Brain::Contact* enemy = NearestEnemy(senses))
        {
            Vector3 toEnemy = enemy->pos - senses.pos;
            toEnemy.y = 0.0f;
            toEnemy /= enemy->dist;

            mem.strafeTimer -= dt;
            if (mem.strafeTimer <= 0.0f)
            {
                mem.strafeSign = -mem.strafeSign;
                mem.strafeTimer = Rand(rng, 1.0f, 2.5f);
            }

            intent.facing = toEnemy;
            intent.move = enemy->dist > kPreferredRange
                              ? toEnemy
                              : Vector3(-toEnemy.z, 0.0f, toEnemy.x) * mem.strafeSign;
            intent.speedScale = kCombatSpeed;
            intent.fire = true;
            intent.fireDist = enemy->dist;
            return intent;
        }

        // Nothing to shoot at: move up. Somewhere is picked at random the way
        // it always was, but only somewhere that stands closer to the enemy's
        // ground than this soldier does now — so a walk with nothing to walk
        // toward still spends itself crossing the map, and a squad that can't
        // see anybody drifts forward instead of milling about its own half.
        //
        // Deliberately not a route: any point that makes progress will do, so
        // the flanks get taken as readily as the middle and a mountain in the
        // way is gone around by soldiers who have never heard of it. What it
        // costs is that a soldier pressed flat against something can pick a
        // point on the far side of it — which the repick timer below answers,
        // the same way it always answered a soldier leaning on a corner.
        mem.repickTimer -= dt;
        const Vector3 toTarget = mem.wanderTarget - senses.pos;
        const float dist = toTarget.Length();
        if (dist < 1.0f || mem.repickTimer <= 0.0f)
        {
            const float marginX = senses.arenaHalf.x - 2.0f;
            const float marginZ = senses.arenaHalf.y - 2.0f;
            const float progress = (senses.objective - senses.pos).Length();
            // A handful of tries, then whatever came up: a soldier standing on
            // the objective has nowhere closer to be, and the walk is the point
            // rather than the arrival.
            for (int tries = 0; tries < 8; ++tries)
            {
                mem.wanderTarget = { Rand(rng, -marginX, marginX), 0.0f,
                                     Rand(rng, -marginZ, marginZ) };
                if ((senses.objective - mem.wanderTarget).Length() < progress)
                    break;
            }
            mem.repickTimer = Rand(rng, 4.0f, 8.0f);
            return intent; // stands still for the frame it's deciding
        }
        intent.move = toTarget / dist;
        intent.facing = intent.move; // walks where it looks, having nothing better to look at
        intent.speedScale = kWanderSpeed;
        return intent;
    }
}

void Brain::Wake(Memory& mem, const Vector3& pos, std::mt19937& rng)
{
    mem.wanderTarget = pos;
    mem.repickTimer = 0.0f; // picks a real one on the first tick
    mem.strafeSign = Rand(rng, 0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
    mem.strafeTimer = Rand(rng, 1.0f, 2.5f);
}

Brain::Intent Brain::Think(Kind kind, Memory& mem, const Senses& senses, float dt,
                           std::mt19937& rng)
{
    switch (kind)
    {
    case Kind::Rifleman: return ThinkRifleman(mem, senses, dt, rng);
    }
    return {};
}
