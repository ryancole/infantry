#include "ClassSelect.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace DirectX;
using namespace ScreenDraw;

namespace
{
    // Screen-space depth layers (ortho z, smaller is closer): card fills at
    // the back, bars in the middle, lines and text on top.
    constexpr float kZCard = 0.8f;
    constexpr float kZBar = 0.7f;
    constexpr float kZText = 0.6f;

    constexpr XMFLOAT4 kTitleColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    constexpr XMFLOAT4 kCardBg = { 0.06f, 0.08f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kCardBgHover = { 0.10f, 0.14f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kBarBg = { 0.14f, 0.17f, 0.23f, 1.0f };

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
    constexpr float kMaxFireRate = MaxSustainedFireRate();
    constexpr float kMaxMoveSpeed = MaxMoveSpeed();
    constexpr float kMaxProjectileSpeed = MaxProjectileSpeed();

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

void ClassSelect::Render(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "CHOOSE YOUR CLASS", w * 0.5f, h * 0.14f, h * 0.05f, kTitleColor);
    DrawCentered(renderer, "CLICK A CARD OR PRESS 1 - 4", w * 0.5f, h * 0.78f, h * 0.022f,
                 kHintColor);
    // The grenade isn't on any card because it isn't a class trait: every
    // soldier gets the same one, so it's called out once, here.
    DrawCentered(renderer, "ONE GRENADE PER LIFE - F TO THROW", w * 0.5f, h * 0.82f, h * 0.022f,
                 kHintColor);
    // The reload isn't a class trait either — every weapon has one — so it's
    // called out alongside the grenade rather than on the cards, which carry
    // only what separates one class from another.
    DrawCentered(renderer, "EMPTY RELOADS ITSELF - R TO RELOAD EARLY", w * 0.5f, h * 0.86f,
                 h * 0.022f, kHintColor);
    // Nor is the blade: every soldier carries the same one, and it's the answer
    // to the range no class's primary covers, so it belongs down here with the
    // rest of what they all have in common.
    DrawCentered(renderer, "THREE MELEE SWINGS, THEN A WAIT - V TO SWING", w * 0.5f, h * 0.90f,
                 h * 0.022f, kHintColor);

    for (size_t i = 0; i < kClassCount; ++i)
    {
        const ClassDef& def = kClassDefs[i];
        const Rect r = CardRect(i, w, h);
        const bool hover = static_cast<int>(i) == m_hover;
        const float pad = r.w * 0.08f;

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZCard, hover ? kCardBgHover : kCardBg);
        AppendOutline(m_lines, r.x, r.y, r.w, r.h, kZText,
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
        char loadout[48];
        std::snprintf(loadout, sizeof(loadout), "%d %s - %.1fS RELOAD", def.primary.magazine,
                      def.primary.magazine == 1 ? "RD" : "RDS", def.primary.reloadTime);
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
            hasAbility ? "Q - " + std::string(def.ability.name) : "NO ABILITY YET";
        DrawCentered(renderer, abilityLine, r.x + r.w * 0.5f, r.y + r.h * 0.505f, r.w * 0.042f,
                     hasAbility ? Dim(def.color, 0.85f) : kHintColor);

        // Stat bars: label + background + class-colored fill.
        struct Bar
        {
            const char* label;
            float fraction;
        };
        const Bar bars[3] = {
            { "SPD", def.move.speed / kMaxMoveSpeed },
            { "ROF", SustainedFireRate(def.primary) / kMaxFireRate },
            { "RNG", def.primary.projectileSpeed / kMaxProjectileSpeed },
        };
        const float rowH = r.h * 0.10f;
        const float labelSize = rowH * 0.55f;
        const float barX = r.x + pad + labelSize * 3.2f;
        const float barW = r.x + r.w - pad - barX;
        for (int b = 0; b < 3; ++b)
        {
            const float rowY = r.y + r.h * 0.56f + b * rowH * 1.35f;
            renderer.DrawScreenText(bars[b].label, r.x + pad, rowY, labelSize, kHintColor);
            AppendQuad(m_tris, barX, rowY, barW, labelSize, kZBar, kBarBg);
            AppendQuad(m_tris, barX, rowY, barW * std::clamp(bars[b].fraction, 0.0f, 1.0f),
                       labelSize, kZBar - 0.02f, def.color);
        }
    }

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
