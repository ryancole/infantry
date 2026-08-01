#include "Hud.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

using namespace DirectX;

namespace
{
    // The cluster is authored against a 1080-tall window and scaled by the real
    // height, so it holds the same share of the screen at any window size
    // instead of drifting out of proportion with the arena.
    constexpr float kDesignHeight = 1080.0f;

    constexpr float kPanelPad = 18.0f;
    constexpr float kPanelCorner = 11.0f;
    constexpr float kBottomMargin = 26.0f;
    constexpr float kModuleGap = 26.0f;
    constexpr float kIconSize = 30.0f;
    constexpr float kIconGap = 11.0f;
    constexpr float kValueSize = 26.0f; // cap height of a module's number
    constexpr float kRowGap = 11.0f;    // icon row -> bar row
    constexpr float kBarHeight = 6.0f;
    constexpr float kPipGap = 3.0f;
    // Above this a magazine is drawn as one continuous bar. Rounds you could
    // count at a glance are worth drawing as rounds; thirty pips would just be
    // a bar with gaps in it, and a noisier one.
    constexpr int kMaxPips = 12;
    constexpr int kHealthSegments = 5; // 20 health each, so a chunk lost is a chunk gone

    constexpr float kHintSize = 13.0f;
    constexpr float kHintGap = 15.0f; // hint row -> panel
    constexpr float kHintKeyPad = 7.0f;
    constexpr float kHintTextGap = 7.0f;
    constexpr float kHintSpacing = 24.0f;

    // Minimum module widths, so the bars underneath health and ammo have room
    // to be read as bars and the two counters stay narrow.
    constexpr float kWideModule = 132.0f;
    constexpr float kNarrowModule = 64.0f;

    constexpr XMFLOAT4 kPanelBg = { 0.035f, 0.050f, 0.075f, 0.78f };
    constexpr XMFLOAT4 kDivider = { 1.00f, 1.00f, 1.00f, 0.07f };
    constexpr XMFLOAT4 kTrack = { 1.00f, 1.00f, 1.00f, 0.10f };
    constexpr XMFLOAT4 kValueColor = { 0.88f, 0.92f, 0.97f, 1.0f };
    constexpr XMFLOAT4 kMutedColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    // Health runs green -> amber -> red so its state carries without reading
    // the number; the thresholds are a hit or two apart at typical damage.
    constexpr XMFLOAT4 kHealthGood = { 0.35f, 0.85f, 0.45f, 1.0f };
    constexpr XMFLOAT4 kHealthWarn = { 0.95f, 0.72f, 0.22f, 1.0f };
    constexpr XMFLOAT4 kHealthLow = { 0.92f, 0.28f, 0.22f, 1.0f };
    constexpr XMFLOAT4 kAmmoColor = { 0.95f, 0.80f, 0.35f, 1.0f };
    constexpr XMFLOAT4 kReloadColor = { 0.95f, 0.52f, 0.18f, 1.0f };
    constexpr XMFLOAT4 kGrenadeColor = { 0.58f, 0.72f, 0.42f, 1.0f };
    constexpr XMFLOAT4 kContactColor = { 0.80f, 0.38f, 0.34f, 1.0f };
    constexpr XMFLOAT4 kSpentColor = { 0.32f, 0.36f, 0.44f, 1.0f }; // an empty slot's icon

    constexpr float kDeadFade = 0.35f; // whole cluster, during the respawn wait

    XMFLOAT4 Fade(const XMFLOAT4& c, float f)
    {
        return { c.x, c.y, c.z, c.w * f };
    }

    // Darkens rgb without touching alpha — for the shaded parts of an icon,
    // which have to stay the same material as the part they sit against.
    XMFLOAT4 Shade(const XMFLOAT4& c, float f)
    {
        return { c.x * f, c.y * f, c.z * f, c.w };
    }

    void AppendQuad(std::vector<Vertex>& out, float x, float y, float w, float h,
                    const XMFLOAT4& c)
    {
        if (w <= 0.0f || h <= 0.0f)
            return;
        const Vertex v[4] = {
            { XMFLOAT3{ x, y, 0.0f }, c },
            { XMFLOAT3{ x + w, y, 0.0f }, c },
            { XMFLOAT3{ x + w, y + h, 0.0f }, c },
            { XMFLOAT3{ x, y + h, 0.0f }, c },
        };
        out.push_back(v[0]);
        out.push_back(v[1]);
        out.push_back(v[2]);
        out.push_back(v[0]);
        out.push_back(v[2]);
        out.push_back(v[3]);
    }

    void AppendTriangle(std::vector<Vertex>& out, float x0, float y0, float x1, float y1, float x2,
                        float y2, const XMFLOAT4& c)
    {
        out.push_back({ XMFLOAT3{ x0, y0, 0.0f }, c });
        out.push_back({ XMFLOAT3{ x1, y1, 0.0f }, c });
        out.push_back({ XMFLOAT3{ x2, y2, 0.0f }, c });
    }

    // Angles are screen-space: y runs down, so they sweep clockwise from +x.
    void AppendFan(std::vector<Vertex>& out, float cx, float cy, float radius, float start,
                   float sweep, int segments, const XMFLOAT4& c)
    {
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = start + sweep * static_cast<float>(i) / segments;
            const float a1 = start + sweep * static_cast<float>(i + 1) / segments;
            AppendTriangle(out, cx, cy, cx + std::cos(a0) * radius, cy + std::sin(a0) * radius,
                           cx + std::cos(a1) * radius, cy + std::sin(a1) * radius, c);
        }
    }

    void AppendDisc(std::vector<Vertex>& out, float cx, float cy, float radius, const XMFLOAT4& c)
    {
        AppendFan(out, cx, cy, radius, 0.0f, XM_2PI, 20, c);
    }

    // Rounded rectangle: three strips plus a quarter-circle at each corner.
    // Everything in the cluster that isn't an icon is made of these, which is
    // most of what separates it from the old text line.
    void AppendRoundRect(std::vector<Vertex>& out, float x, float y, float w, float h, float r,
                         const XMFLOAT4& c)
    {
        if (w <= 0.0f || h <= 0.0f)
            return;
        r = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
        if (r <= 0.0f)
        {
            AppendQuad(out, x, y, w, h, c);
            return;
        }

        AppendQuad(out, x + r, y, w - 2.0f * r, h, c);
        AppendQuad(out, x, y + r, r, h - 2.0f * r, c);
        AppendQuad(out, x + w - r, y + r, r, h - 2.0f * r, c);

        constexpr int kCornerSegments = 5;
        AppendFan(out, x + r, y + r, r, XM_PI, XM_PIDIV2, kCornerSegments, c);
        AppendFan(out, x + w - r, y + r, r, -XM_PIDIV2, XM_PIDIV2, kCornerSegments, c);
        AppendFan(out, x + w - r, y + h - r, r, 0.0f, XM_PIDIV2, kCornerSegments, c);
        AppendFan(out, x + r, y + h - r, r, XM_PIDIV2, XM_PIDIV2, kCornerSegments, c);
    }

    // --- Icons ---
    //
    // Each draws inside a square box at (x, y) of edge `size`, in one color it
    // shades itself from. They're built from the same primitives as the rest of
    // the panel rather than from a texture: at this size a handful of triangles
    // reads as cleanly as a sprite would, and nothing has to be authored,
    // shipped, or kept in step with the window's resolution.

    // A medical cross. Reads as health at a glance in a way a number never does.
    void DrawHealthIcon(std::vector<Vertex>& out, float x, float y, float size, const XMFLOAT4& c)
    {
        const float t = size * 0.34f;
        const float r = t * 0.22f;
        AppendRoundRect(out, x + (size - t) * 0.5f, y, t, size, r, c);
        AppendRoundRect(out, x, y + (size - t) * 0.5f, size, t, r, c);
    }

    // A cartridge standing on its base: pointed jacket over a brass case, with
    // the rim flared at the bottom so it reads as a round rather than a pin.
    void DrawAmmoIcon(std::vector<Vertex>& out, float x, float y, float size, const XMFLOAT4& c)
    {
        const float bw = size * 0.46f;
        const float bx = x + (size - bw) * 0.5f;

        AppendTriangle(out, x + size * 0.5f, y, bx, y + size * 0.36f, bx + bw, y + size * 0.36f,
                       Shade(c, 1.0f));
        AppendRoundRect(out, bx, y + size * 0.32f, bw, size * 0.56f, bw * 0.16f, Shade(c, 0.72f));
        AppendRoundRect(out, bx - size * 0.05f, y + size * 0.84f, bw + size * 0.10f, size * 0.16f,
                        size * 0.04f, Shade(c, 0.55f));
    }

    // A fragmentation grenade: body, banded waist, and the lever off the neck.
    void DrawGrenadeIcon(std::vector<Vertex>& out, float x, float y, float size, const XMFLOAT4& c)
    {
        AppendRoundRect(out, x + size * 0.40f, y + size * 0.14f, size * 0.20f, size * 0.24f,
                        size * 0.05f, Shade(c, 0.80f));
        AppendQuad(out, x + size * 0.58f, y + size * 0.16f, size * 0.30f, size * 0.08f,
                   Shade(c, 0.65f));
        AppendQuad(out, x + size * 0.80f, y + size * 0.16f, size * 0.08f, size * 0.26f,
                   Shade(c, 0.65f));
        AppendDisc(out, x + size * 0.50f, y + size * 0.64f, size * 0.33f, c);
        AppendQuad(out, x + size * 0.19f, y + size * 0.60f, size * 0.62f, size * 0.06f,
                   Shade(c, 0.62f));
    }

    // A helmeted head and shoulders: whoever else is on the field.
    void DrawContactIcon(std::vector<Vertex>& out, float x, float y, float size, const XMFLOAT4& c)
    {
        AppendDisc(out, x + size * 0.50f, y + size * 0.27f, size * 0.20f, c);
        AppendRoundRect(out, x + size * 0.13f, y + size * 0.53f, size * 0.74f, size * 0.47f,
                        size * 0.20f, Shade(c, 0.82f));
    }

    enum class Icon
    {
        Health,
        Ammo,
        Grenade,
        Contact,
    };

    void DrawIcon(std::vector<Vertex>& out, Icon icon, float x, float y, float size,
                  const XMFLOAT4& c)
    {
        switch (icon)
        {
        case Icon::Health: DrawHealthIcon(out, x, y, size, c); break;
        case Icon::Ammo: DrawAmmoIcon(out, x, y, size, c); break;
        case Icon::Grenade: DrawGrenadeIcon(out, x, y, size, c); break;
        case Icon::Contact: DrawContactIcon(out, x, y, size, c); break;
        }
    }

    // What sits under a module's number, if anything: a segmented meter (health),
    // one pip per round (small magazines), or a plain fill (big magazines and
    // the reload sweep).
    enum class Bar
    {
        None,
        Fill,
        Pips,
        Segments,
    };

    struct Module
    {
        Icon icon;
        XMFLOAT4 iconColor;
        std::string value;
        XMFLOAT4 valueColor;
        Bar bar = Bar::None;
        float fill = 0.0f; // 0..1, for Fill and Segments
        int pips = 0;      // slot count, for Pips
        int pipsFull = 0;
        XMFLOAT4 barColor = kTrack;
        float minWidth = kNarrowModule;
        float width = 0.0f; // filled in during layout
    };

    struct Hint
    {
        const char* key;
        const char* label;
    };
    constexpr Hint kHints[] = {
        { "R", "RELOAD" },
        { "F", "GRENADE" },
        { "N", "SPAWN NPC" },
    };
}

void Hud::Render(Renderer& renderer, const State& st)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    const float s = h / kDesignHeight;
    const float fade = st.alive ? 1.0f : kDeadFade;

    // --- Modules ---

    const float hpFrac =
        st.maxHp > 0.0f ? std::clamp(st.hp / st.maxHp, 0.0f, 1.0f) : 0.0f;
    const XMFLOAT4 hpColor =
        hpFrac > 0.5f ? kHealthGood : (hpFrac > 0.25f ? kHealthWarn : kHealthLow);

    Module health;
    health.icon = Icon::Health;
    health.iconColor = hpColor;
    health.value = std::to_string(static_cast<int>(std::ceil(std::max(st.hp, 0.0f))));
    health.valueColor = kValueColor;
    health.bar = Bar::Segments;
    health.fill = hpFrac;
    health.barColor = hpColor;
    health.minWidth = kWideModule;

    // While the magazine is out the count still reads honestly — it's zero, and
    // the trigger is dead — and the bar becomes the wait, filling in the
    // reload's color. The old HUD spelled out RELOADING and deliberately left
    // the remaining time unsaid, on the grounds that a number to read would
    // arrive after it mattered. A bar isn't a number: it's the same glance as
    // everything else here, so the wait can be seen through rather than read.
    const bool reloading = st.reloadFraction >= 0.0f;

    Module ammo;
    ammo.icon = Icon::Ammo;
    ammo.iconColor = reloading ? kReloadColor : kAmmoColor;
    ammo.value = st.magazine > 0 ? std::to_string(st.ammo) + "/" + std::to_string(st.magazine)
                                 : std::to_string(st.ammo);
    ammo.valueColor = reloading ? kMutedColor : kValueColor;
    ammo.barColor = reloading ? kReloadColor : kAmmoColor;
    ammo.minWidth = kWideModule;
    if (reloading)
    {
        ammo.bar = Bar::Fill;
        ammo.fill = std::clamp(st.reloadFraction, 0.0f, 1.0f);
    }
    else if (st.magazine > 0 && st.magazine <= kMaxPips)
    {
        ammo.bar = Bar::Pips;
        ammo.pips = st.magazine;
        ammo.pipsFull = std::clamp(st.ammo, 0, st.magazine);
    }
    else if (st.magazine > 0)
    {
        ammo.bar = Bar::Fill;
        ammo.fill = std::clamp(static_cast<float>(st.ammo) / static_cast<float>(st.magazine), 0.0f,
                               1.0f);
    }

    // Spent equipment greys out rather than disappearing: the slot is still
    // part of the loadout, it just has nothing in it until the next life.
    Module grenade;
    grenade.icon = Icon::Grenade;
    grenade.iconColor = st.grenades > 0 ? kGrenadeColor : kSpentColor;
    grenade.value = std::to_string(st.grenades);
    grenade.valueColor = st.grenades > 0 ? kValueColor : kMutedColor;

    Module contacts;
    contacts.icon = Icon::Contact;
    contacts.iconColor = st.npcs > 0 ? kContactColor : kSpentColor;
    contacts.value = std::to_string(st.npcs);
    contacts.valueColor = st.npcs > 0 ? kValueColor : kMutedColor;

    Module modules[] = { health, ammo, grenade, contacts };
    constexpr size_t kModuleCount = std::size(modules);

    // --- Layout ---

    const float iconSize = kIconSize * s;
    const float valueSize = kValueSize * s;
    const float pad = kPanelPad * s;

    float contentW = 0.0f;
    for (Module& m : modules)
    {
        const float textW = renderer.MeasureScreenText(m.value, valueSize);
        m.width = std::max(m.minWidth * s, iconSize + kIconGap * s + textW);
        contentW += m.width;
    }
    contentW += kModuleGap * s * (kModuleCount - 1);

    const float panelW = contentW + pad * 2.0f;
    const float panelH = pad * 2.0f + iconSize + kRowGap * s + kBarHeight * s;
    const float panelX = std::floor((w - panelW) * 0.5f);
    const float panelY = std::floor(h - kBottomMargin * s - panelH);

    // Rebuilt every frame, but its size barely moves, so the buffers settle at
    // capacity and stop allocating. The HUD is drawn once per frame from the
    // render thread, which is what makes keeping them here safe.
    static std::vector<Vertex> tris;
    tris.clear();

    // --- Panel ---

    AppendRoundRect(tris, panelX, panelY, panelW, panelH, kPanelCorner * s,
                    Fade(kPanelBg, fade));
    // A hairline of the class color along the top edge: the one place the
    // cluster says which soldier this is, and it costs no room to say it.
    AppendQuad(tris, panelX + kPanelCorner * s, panelY, panelW - kPanelCorner * s * 2.0f,
               std::max(2.0f * s, 1.0f), Fade(Shade(st.accent, 0.9f), 0.55f * fade));

    // --- Modules ---

    const float rowY = panelY + pad;
    const float barY = rowY + iconSize + kRowGap * s;
    const float barH = kBarHeight * s;
    float x = panelX + pad;

    for (size_t i = 0; i < kModuleCount; ++i)
    {
        const Module& m = modules[i];

        if (i > 0)
        {
            const float divX = x - kModuleGap * s * 0.5f;
            AppendQuad(tris, divX, panelY + pad * 0.55f, std::max(1.0f * s, 1.0f),
                       panelH - pad * 1.10f, Fade(kDivider, fade));
        }

        DrawIcon(tris, m.icon, x, rowY, iconSize, Fade(m.iconColor, fade));
        renderer.DrawScreenText(m.value, x + iconSize + kIconGap * s,
                                rowY + (iconSize - valueSize) * 0.5f, valueSize,
                                Fade(m.valueColor, fade));

        switch (m.bar)
        {
        case Bar::None:
            break;
        case Bar::Fill:
            AppendRoundRect(tris, x, barY, m.width, barH, barH * 0.5f, Fade(kTrack, fade));
            AppendRoundRect(tris, x, barY, m.width * m.fill, barH, barH * 0.5f,
                            Fade(m.barColor, fade));
            break;
        case Bar::Pips:
        {
            const float pipW = (m.width - kPipGap * s * (m.pips - 1)) / m.pips;
            for (int p = 0; p < m.pips; ++p)
            {
                const float pipX = x + p * (pipW + kPipGap * s);
                AppendRoundRect(tris, pipX, barY, pipW, barH, barH * 0.35f,
                                Fade(p < m.pipsFull ? m.barColor : kTrack, fade));
            }
            break;
        }
        case Bar::Segments:
        {
            // Fixed-size chunks rather than pips: health isn't counted out in
            // units the way rounds are, but a meter that empties in steps still
            // says how much of it is gone without reading the number.
            const float segW = (m.width - kPipGap * s * (kHealthSegments - 1)) / kHealthSegments;
            for (int seg = 0; seg < kHealthSegments; ++seg)
            {
                const float segX = x + seg * (segW + kPipGap * s);
                AppendRoundRect(tris, segX, barY, segW, barH, barH * 0.35f, Fade(kTrack, fade));
                const float segFill =
                    std::clamp(m.fill * kHealthSegments - static_cast<float>(seg), 0.0f, 1.0f);
                AppendRoundRect(tris, segX, barY, segW * segFill, barH, barH * 0.35f,
                                Fade(m.barColor, fade));
            }
            break;
        }
        }

        x += m.width + kModuleGap * s;
    }

    // --- Key hints ---
    //
    // Above the panel as small key caps instead of the old right-aligned line
    // of text: same three bindings, centered with everything else, and short
    // enough to stop competing with the readouts for the eye.

    const float hintSize = kHintSize * s;
    const float capH = hintSize + kHintKeyPad * s * 2.0f;
    const float hintY = panelY - kHintGap * s - capH;

    float hintsW = 0.0f;
    float capWidths[std::size(kHints)] = {};
    for (size_t i = 0; i < std::size(kHints); ++i)
    {
        capWidths[i] = std::max(renderer.MeasureScreenText(kHints[i].key, hintSize) +
                                    kHintKeyPad * s * 2.0f,
                                capH);
        hintsW += capWidths[i] + kHintTextGap * s +
                  renderer.MeasureScreenText(kHints[i].label, hintSize);
    }
    hintsW += kHintSpacing * s * (std::size(kHints) - 1);

    float hintX = std::floor((w - hintsW) * 0.5f);
    for (size_t i = 0; i < std::size(kHints); ++i)
    {
        AppendRoundRect(tris, hintX, hintY, capWidths[i], capH, capH * 0.28f,
                        Fade(kTrack, fade));
        renderer.DrawScreenText(kHints[i].key,
                                hintX + (capWidths[i] -
                                         renderer.MeasureScreenText(kHints[i].key, hintSize)) *
                                            0.5f,
                                hintY + kHintKeyPad * s, hintSize, Fade(kValueColor, fade));

        hintX += capWidths[i] + kHintTextGap * s;
        renderer.DrawScreenText(kHints[i].label, hintX, hintY + (capH - hintSize) * 0.5f, hintSize,
                                Fade(kMutedColor, fade));
        hintX += renderer.MeasureScreenText(kHints[i].label, hintSize) + kHintSpacing * s;
    }

    renderer.DrawScreenTriangles(tris.data(), static_cast<uint32_t>(tris.size()));
}
