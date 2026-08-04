#include "ClassSelect.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace DirectX;
using namespace ScreenDraw;

namespace
{
    // Overlay geometry ignores z — what's submitted later is what's on top —
    // so everything below is appended in the order it should be read: the
    // backdrop, then each card's fill, then its border and bars.
    constexpr float kZ = 0.0f;

    constexpr XMFLOAT4 kTitleColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    constexpr XMFLOAT4 kCardBg = { 0.06f, 0.08f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kCardBgHover = { 0.10f, 0.14f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kBarBg = { 0.14f, 0.17f, 0.23f, 1.0f };
    // What the match behind the cards is dimmed to when this screen is opened
    // from inside one. Not blacked out: the fight is still going on and being
    // able to see it is half of why a player is choosing a class at all.
    constexpr XMFLOAT4 kBackdrop = { 0.02f, 0.03f, 0.05f, 0.76f };

    // A rectangle's border as four quads. The outlines used to be lines in a
    // batch of their own, which the overlay path has no equivalent of; drawn
    // this way they also thicken with the resolution instead of staying one
    // pixel on every screen there is.
    void AppendBorder(std::vector<Vertex>& out, float x, float y, float w, float h, float t,
                      const XMFLOAT4& color)
    {
        AppendQuad(out, x, y, w, t, kZ, color);
        AppendQuad(out, x, y + h - t, w, t, kZ, color);
        AppendQuad(out, x, y + t, t, h - t * 2.0f, kZ, color);
        AppendQuad(out, x + w - t, y + t, t, h - t * 2.0f, kZ, color);
    }

    // Normalizers for the stat bars: whatever the strongest class in a stat
    // has, that's a full bar. All three are folded out of the class table
    // rather than written down. A hand-copied maximum is a second place the
    // balance lives, and it fails quietly in both directions — raise a class
    // past it and the fill runs off the end of its background, drop the class
    // that set it and nothing on the screen ever reaches full again. Neither
    // shows up anywhere near the table being edited.
    constexpr float MaxSustainedFireRate()
    {
        float best = 0.0f;
        for (const ClassDef& def : kClassDefs)
            best = std::max(best, SustainedFireRate(def.primary));
        return best;
    }
    constexpr float MaxMoveSpeed()
    {
        float best = 0.0f;
        for (const ClassDef& def : kClassDefs)
            best = std::max(best, def.move.speed);
        return best;
    }
    constexpr float MaxProjectileSpeed()
    {
        float best = 0.0f;
        for (const ClassDef& def : kClassDefs)
            best = std::max(best, def.primary.projectileSpeed);
        return best;
    }
    constexpr float MaxSight()
    {
        float best = 0.0f;
        for (const ClassDef& def : kClassDefs)
            best = std::max(best, def.sight);
        return best;
    }
    constexpr float kMaxFireRate = MaxSustainedFireRate();
    constexpr float kMaxMoveSpeed = MaxMoveSpeed();
    constexpr float kMaxProjectileSpeed = MaxProjectileSpeed();
    constexpr float kMaxSight = MaxSight();

}

ClassSelect::Rect ClassSelect::CardRect(size_t index, float width, float height)
{
    const float cardW = width * 0.20f;
    const float gap = width * 0.02f;
    const float total = kClassCount * cardW + (kClassCount - 1) * gap;
    return { (width - total) * 0.5f + index * (cardW + gap), height * 0.30f, cardW,
             height * 0.42f };
}

std::optional<ClassId> ClassSelect::Update(const Input& input, uint32_t width, uint32_t height)
{
    for (size_t i = 0; i < kClassCount; ++i)
        if (input.KeyPressed('1' + static_cast<int>(i)))
            return static_cast<ClassId>(i);

    using PadTracker = DirectX::GamePad::ButtonStateTracker;
    const PadTracker::ButtonState padButtons[kClassCount] = {
        input.padEvents.a, input.padEvents.b, input.padEvents.x, input.padEvents.y
    };
    for (size_t i = 0; i < kClassCount; ++i)
        if (padButtons[i] == PadTracker::PRESSED)
            return static_cast<ClassId>(i);

    m_hover = -1;
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (size_t i = 0; i < kClassCount; ++i)
    {
        if (CardRect(i, w, h).Contains(input.mouseX, input.mouseY))
        {
            m_hover = static_cast<int>(i);
            if (input.MousePressed(0))
                return static_cast<ClassId>(i);
        }
    }
    return std::nullopt;
}

void ClassSelect::Render(Renderer& renderer, const Bindings& binds, Mode mode)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());

    m_tris.clear();

    // Over a match, the arena goes down under a sheet first so the cards read
    // against something quiet. Before one there is nothing back there to dim.
    if (mode == Mode::Change)
        AppendQuad(m_tris, 0.0f, 0.0f, w, h, kZ, kBackdrop);

    // What the screen is for, which is not the same sentence twice: the first
    // time it's the choice that starts a match, and the second it's the choice
    // being revisited. A player who opened this by accident should be able to
    // tell which one they're looking at from the top line alone.
    DrawCentered(renderer, mode == Mode::Change ? "CHANGE CLASS" : "CHOOSE YOUR CLASS", w * 0.5f,
                 h * 0.14f, h * 0.05f, kTitleColor);

    if (mode == Mode::Change)
    {
        // One line, tucked under the cards rather than down the bottom of the
        // screen: the match's own readouts are still on, and the loadout
        // cluster lives where the briefing below would otherwise be. The
        // briefing isn't repeated anyway — a player already in a match has
        // spawned once and been told about the grenade, the reload, and the
        // blade.
        DrawCentered(renderer, "CLICK A CARD OR PRESS 1 - 4 - ESC TO KEEP THIS ONE", w * 0.5f,
                     h * 0.755f, h * 0.022f, kHintColor);
    }
    else
    {
        // The card numbers are the one thing on this screen that isn't a
        // binding: picking a class is a menu, and menus stay where the game put
        // them.
        DrawCentered(renderer, "CLICK A CARD OR PRESS 1 - 4", w * 0.5f, h * 0.78f, h * 0.022f,
                     kHintColor);
        // The grenade isn't on any card because it isn't a class trait: every
        // soldier gets the same one, so it's called out once, here. The key
        // comes off the player's bindings rather than being spelled out — this
        // screen is the last thing read before spawning, and a briefing naming
        // a key the player doesn't use would be worse than no briefing.
        DrawCentered(renderer, "ONE GRENADE PER LIFE - " +
                                   binds.Label(Bindings::Action::Grenade) + " TO THROW",
                     w * 0.5f, h * 0.82f, h * 0.022f, kHintColor);
        // The reload isn't a class trait either — every weapon has one — so
        // it's called out alongside the grenade rather than on the cards, which
        // carry only what separates one class from another. What the cards do
        // carry is the two prices; what this line has to say is why there are
        // two, since a player who doesn't know reloading early is cheaper will
        // only ever pay the other one.
        DrawCentered(renderer, "EMPTY RELOADS ITSELF - " +
                                   binds.Label(Bindings::Action::Reload) +
                                   " TO RELOAD EARLY, AND QUICKER",
                     w * 0.5f, h * 0.86f, h * 0.022f, kHintColor);
        // Nor is the blade: every soldier carries the same one, and it's the
        // answer to the range no class's primary covers, so it belongs down
        // here with the rest of what they all have in common.
        DrawCentered(renderer, "THREE MELEE SWINGS, THEN A WAIT - " +
                                   binds.Label(Bindings::Action::Melee) + " TO SWING",
                     w * 0.5f, h * 0.90f, h * 0.022f, kHintColor);
    }

    for (size_t i = 0; i < kClassCount; ++i)
    {
        const ClassDef& def = kClassDefs[i];
        const Rect r = CardRect(i, w, h);
        const bool hover = static_cast<int>(i) == m_hover;
        const float pad = r.w * 0.08f;

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZ, hover ? kCardBgHover : kCardBg);
        AppendBorder(m_tris, r.x, r.y, r.w, r.h, std::max(h * 0.0018f, 1.0f),
                     hover ? def.color : Dim(def.color, 0.45f));

        const std::string key = std::to_string(i + 1);
        renderer.DrawScreenText(key, r.x + pad, r.y + pad, r.h * 0.06f, kHintColor);

        DrawCentered(renderer, def.name, r.x + r.w * 0.5f, r.y + r.h * 0.20f, r.w * 0.095f,
                     def.color);
        DrawCentered(renderer, def.blurb, r.x + r.w * 0.5f, r.y + r.h * 0.36f, r.w * 0.042f,
                     kHintColor);

        // Magazine and reload, spelled out rather than barred: they're the two
        // halves of one trade (how long the class can fire, what the pause
        // costs), and a bar per half would read as two more things to be good
        // at when a big magazine and a quick reload aren't the same virtue.
        //
        // Both reload prices, early first, in the order the player will meet
        // them. A single-shot weapon gets one number because it only ever has
        // one: its magazine is always empty by the time it's changed, and
        // printing a price it can't be charged would be a lie about the class
        // on the screen where the class is being chosen.
        //
        // A minimum range rides on the end of the same line, for the one class
        // that has one. It's spelled out rather than barred for a reason of its
        // own: the RNG bar underneath is a reading of muzzle speed, so a notch
        // cut in its near end would be drawn against the wrong axis and would
        // lie about how much of the class's reach was missing. And it can't be
        // left off the card, because dead ground is exactly what this screen is
        // for — the one thing that makes the sniper a different soldier to pick
        // rather than a longer version of the marine.
        char loadout[64];
        int n;
        if (def.primary.magazine == 1)
            n = std::snprintf(loadout, sizeof(loadout), "1 RD - %.1fS RELOAD",
                              def.primary.reloadEmpty);
        else
            n = std::snprintf(loadout, sizeof(loadout), "%d RDS - %.1f/%.1fS RELOAD",
                              def.primary.magazine, def.primary.reloadEarly,
                              def.primary.reloadEmpty);
        if (def.primary.minRange > 0.0f && n > 0 && n < static_cast<int>(sizeof(loadout)))
            std::snprintf(loadout + n, sizeof(loadout) - n, " - %.0f MIN RANGE",
                          def.primary.minRange);
        DrawCentered(renderer, loadout, r.x + r.w * 0.5f, r.y + r.h * 0.45f, r.w * 0.042f,
                     Dim(def.color, 0.85f));

        // The ability, named rather than barred: it's the one line on the card
        // that isn't a quantity, and two classes with identical bars are still
        // different soldiers if only one of them can put itself back together.
        // Classes without one say so outright — a blank row would read as a
        // card that failed to draw, and "not yet" is the truth about where the
        // prototype is rather than something to hide.
        const bool hasAbility = def.ability.kind != Ability::Kind::None;
        const std::string abilityLine =
            hasAbility ? binds.Label(Bindings::Action::Ability) + " - " + def.ability.name
                       : "NO ABILITY YET";
        DrawCentered(renderer, abilityLine, r.x + r.w * 0.5f, r.y + r.h * 0.505f, r.w * 0.042f,
                     hasAbility ? Dim(def.color, 0.85f) : kHintColor);

        // Stat bars: label + background + class-colored fill.
        struct Bar
        {
            const char* label;
            float fraction;
        };
        // LOS last, and deliberately next to RNG: the two are the class's two
        // halves of the same question, and the pair is only interesting when
        // they disagree. A short bar under a long one is a class that shoots
        // further than it can see and needs telling where to aim; the sniper is
        // the one card where both run long, which is what the class is for.
        const Bar bars[4] = {
            { "SPD", def.move.speed / kMaxMoveSpeed },
            { "ROF", SustainedFireRate(def.primary) / kMaxFireRate },
            { "RNG", def.primary.projectileSpeed / kMaxProjectileSpeed },
            { "LOS", def.sight / kMaxSight },
        };
        const float rowH = r.h * 0.10f;
        const float labelSize = rowH * 0.55f;
        const float barX = r.x + pad + labelSize * 3.2f;
        const float barW = r.x + r.w - pad - barX;
        for (int b = 0; b < 4; ++b)
        {
            // Four rows where there were three, tightened to fit rather than
            // spilling past the card: the block still starts under the ability
            // line and now ends just short of the bottom edge.
            const float rowY = r.y + r.h * 0.55f + b * rowH * 1.15f;
            renderer.DrawScreenText(bars[b].label, r.x + pad, rowY, labelSize, kHintColor);
            AppendQuad(m_tris, barX, rowY, barW, labelSize, kZ, kBarBg);
            AppendQuad(m_tris, barX, rowY, barW * std::clamp(bars[b].fraction, 0.0f, 1.0f),
                       labelSize, kZ, def.color);
        }
    }

    renderer.DrawScreenTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()));
}
