#include "Ability.h"

#include "Hud.h"

#include <algorithm>
#include <cmath>

// The drawn half of an ability: the marks it leaves on the world and what it
// lets its owner see. Split from Ability.cpp along the same line the whole
// game is being split along — the rules run wherever the simulation runs,
// including a server with no screen, while these functions only mean anything
// on a machine with a player looking at it. The header stays one file because
// an ability is still one idea; which half of it you're holding is said by
// which functions you call.

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    // --- The dressing's marks ---

    // A ring closing round the user's feet over the ability's whole duration.
    // Deliberately something an enemy can read as well as the player: a medic
    // patching up is a medic who can't shoot back for the next two seconds, and
    // that's worth knowing from across the arena.
    constexpr XMFLOAT4 kRingColor = { 0.35f, 0.85f, 0.70f, 0.75f };
    constexpr float kRingRadius = 0.9f;  // just clear of the soldier's shoulders
    constexpr float kRingHeight = 0.05f; // above the fog quads, so it isn't dimmed
    // The link to whoever is being treated. Brighter than anything else on the
    // floor, since this is the one soldier the dressing is actually reaching.
    constexpr XMFLOAT4 kLinkColor = { 0.45f, 0.95f, 0.78f, 0.80f };
    constexpr float kLinkHeight = 0.75f; // chest height, so it reads as a line between two people

    // Squadmate health bars. Small: it's a glance that has to survive several of
    // them being on screen at once, and a soldier is 0.8 wide, so a bar a little
    // narrower than that reads as belonging to the body under it.
    constexpr float kBarHeight = 1.70f; // clear of the helmet (the head sits at 1.05)
    constexpr float kBarWidth = 0.70f;
    constexpr float kBarThickness = 0.11f;
    constexpr float kBarEdge = 0.02f; // dark surround, so a red bar still reads against blood
    constexpr XMFLOAT4 kBarBack = { 0.03f, 0.04f, 0.06f, 0.72f };

    // A horizontal run of line segments on a circle, `sweep` radians wide from
    // `start`. Angles are world headings: measured from +x toward +z, the same
    // convention atan2(z, x) gives back.
    void AppendArc(std::vector<Vertex>& out, const Vector3& center, float radius, float start,
                   float sweep, int segments, const XMFLOAT4& color)
    {
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = start + sweep * i / segments;
            const float a1 = start + sweep * (i + 1) / segments;
            out.push_back({ XMFLOAT3{ center.x + std::cos(a0) * radius, center.y,
                                      center.z + std::sin(a0) * radius }, color });
            out.push_back({ XMFLOAT3{ center.x + std::cos(a1) * radius, center.y,
                                      center.z + std::sin(a1) * radius }, color });
        }
    }

    // An upright quad in the world, `origin` its bottom-left corner, running
    // `width` along `right` and `height` straight up. Billboarding the cheap
    // way: the horizontal axis follows the camera and the vertical one is simply
    // world up, which under an orthographic camera that never rolls is all it
    // takes to stand something square to the screen. A true billboard would also
    // tilt back by the camera's pitch; at this size the difference is a bar that
    // looks very slightly foreshortened, which is the right amount of effort to
    // spend on it.
    void AppendUprightQuad(std::vector<Vertex>& out, const Vector3& origin, const Vector3& right,
                           float width, float height, const XMFLOAT4& color)
    {
        if (width <= 0.0f || height <= 0.0f)
            return;
        const Vector3 across = right * width;
        const Vector3 up(0.0f, height, 0.0f);
        const XMFLOAT3 p0 = origin;
        const XMFLOAT3 p1 = origin + across;
        const XMFLOAT3 p2 = origin + across + up;
        const XMFLOAT3 p3 = origin + up;
        out.push_back({ p0, color });
        out.push_back({ p1, color });
        out.push_back({ p2, color });
        out.push_back({ p0, color });
        out.push_back({ p2, color });
        out.push_back({ p3, color });
    }

    // Squadmate health, over the heads of the ones who have lost some. Only the
    // wounded get a bar: "who should I heal" is the question being answered, and
    // a row of full bars would bury the answer in soldiers who aren't part of
    // it. It also means the bar going away is what says a treatment finished.
    void AppendHealVision(const Ability::Scene& scene, const Vector3& screenRight,
                          std::vector<Vertex>& tris)
    {
        if (!scene.allies)
            return;
        for (const Ability::Target& ally : *scene.allies)
        {
            if (*ally.hp <= 0.0f || *ally.hp >= scene.maxHp)
                continue;
            const float frac = std::clamp(*ally.hp / scene.maxHp, 0.0f, 1.0f);
            const Vector3 left(ally.pos.x - screenRight.x * kBarWidth * 0.5f, kBarHeight,
                               ally.pos.z - screenRight.z * kBarWidth * 0.5f);
            // Backing plate first, a hair proud of the bar on every side, so a
            // nearly-empty red bar doesn't have to be read against whatever
            // happens to be behind it.
            AppendUprightQuad(tris, left - screenRight * kBarEdge - Vector3(0.0f, kBarEdge, 0.0f),
                              screenRight, kBarWidth + kBarEdge * 2.0f,
                              kBarThickness + kBarEdge * 2.0f, kBarBack);
            // The same green-amber-red the user's own health reads in: a
            // squadmate at a third has to look like the player at a third.
            AppendUprightQuad(tris, left, screenRight, kBarWidth * frac, kBarThickness,
                              Hud::HealthColor(frac));
        }
    }
}

void Ability::AppendIndicator(const Def& def, const Runtime& rt, const Vector3& userPos,
                              std::vector<Vertex>& lines)
{
    if (rt.time <= 0.0f || def.duration <= 0.0f)
        return;

    // A ring closing round the user's feet over the duration. Unlike the melee
    // arc this is a promise rather than a record: it says how much of the thing
    // is left to run, which is the only number that matters while it runs — for
    // the player deciding whether they can afford the rest of it, and for
    // anyone watching who now knows exactly how long this soldier can't shoot
    // back.
    const float done = 1.0f - rt.time / def.duration;
    AppendArc(lines, { userPos.x, kRingHeight, userPos.z }, kRingRadius, -XM_PIDIV2, XM_2PI * done,
              28, kRingColor);

    // And who it's reaching, if anyone: a line between the two of them at chest
    // height with a ring around the far end. Since the target is picked again
    // every frame, this is also the only thing that tells the player they have
    // drifted off the soldier they meant to be treating — the health bar
    // climbing is somebody else's, and it isn't on their screen.
    if (!rt.reached)
        return;
    lines.push_back({ XMFLOAT3{ userPos.x, kLinkHeight, userPos.z }, kLinkColor });
    lines.push_back({ XMFLOAT3{ rt.reachedPos.x, kLinkHeight, rt.reachedPos.z }, kLinkColor });
    AppendArc(lines, { rt.reachedPos.x, kRingHeight, rt.reachedPos.z }, kRingRadius, 0.0f, XM_2PI,
              24, kLinkColor);
}

void Ability::AppendVision(const Def& def, const Scene& scene, const Vector3& screenRight,
                           std::vector<Vertex>& tris)
{
    switch (def.kind)
    {
    case Kind::None: break;
    case Kind::Heal: AppendHealVision(scene, screenRight, tris); break;
    }
}
