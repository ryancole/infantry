#include "Hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

    // The roster panel in the top-right corner. Its own scale of everything,
    // one notch down from the loadout's: it's the secondary readout on the
    // screen and drawing it at the same weight would make the eye choose.
    constexpr float kTopMargin = 26.0f;
    constexpr float kSideMargin = 26.0f;
    constexpr float kRosterIconSize = 24.0f;
    constexpr float kRosterValueSize = 21.0f;
    constexpr float kRosterGap = 10.0f;    // icon -> count -> pips -> score
    constexpr float kRosterRowGap = 12.0f; // one side's row -> the other's
    constexpr float kRosterPipsWidth = 68.0f;
    // The score is the number the match ends on, so it's the biggest thing in
    // the panel and the clock above it is a notch under: what's left to play
    // for, and how long there is to play for it.
    constexpr float kRosterScoreSize = 27.0f;
    constexpr float kRosterClockSize = 23.0f;
    constexpr float kRosterHeadGap = 11.0f; // clock -> the rule under it -> the rows
    // The last minute of a match turns the clock the same orange every other
    // wait in the cluster is drawn in. It's the one number here that changes
    // how the fight should be played, and only right at the end.
    constexpr float kClockWarning = 60.0f;

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
    // (Hud::HealthColor is the one place that applies them.)
    constexpr XMFLOAT4 kHealthGood = { 0.35f, 0.85f, 0.45f, 1.0f };
    constexpr XMFLOAT4 kHealthWarn = { 0.95f, 0.72f, 0.22f, 1.0f };
    constexpr XMFLOAT4 kHealthLow = { 0.92f, 0.28f, 0.22f, 1.0f };
    constexpr XMFLOAT4 kAmmoColor = { 0.95f, 0.80f, 0.35f, 1.0f };
    constexpr XMFLOAT4 kReloadColor = { 0.95f, 0.52f, 0.18f, 1.0f };
    constexpr XMFLOAT4 kMeleeColor = { 0.66f, 0.80f, 0.94f, 1.0f }; // bare steel
    constexpr XMFLOAT4 kGrenadeColor = { 0.58f, 0.72f, 0.42f, 1.0f };
    // Greener than the ammo and bluer than the health meter, so the ability
    // module doesn't read as either of the things it sits between. Matches the
    // ring the game draws round a soldier using it.
    constexpr XMFLOAT4 kAbilityColor = { 0.35f, 0.85f, 0.70f, 1.0f };
    // The two sides come in off the game (State::allyColor / enemyColor) in the
    // colors their armor is painted, so friend or foe is answered the same way
    // whether it's read off a body or out of the corner. Armor colors are
    // chosen to hold up at arena distance, though, and a 12-pixel icon on a
    // near-black panel wants more light than that — so they're mixed a quarter
    // of the way to white on the way in. Hue is untouched; nothing here is
    // allowed to change which side a color means.
    constexpr float kSideLift = 0.25f;
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

    // Mixes `f` of the way to white. Unlike Shade, which scales toward black
    // and eventually loses a dark color entirely, this can only make a color
    // easier to see — which is what a small mark on a dark panel needs.
    XMFLOAT4 Lift(const XMFLOAT4& c, float f)
    {
        return { c.x + (1.0f - c.x) * f, c.y + (1.0f - c.y) * f, c.z + (1.0f - c.z) * f, c.w };
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

    // A quad that isn't axis-aligned: centered on (x, y) at one end, running
    // `length` along the unit direction (dx, dy) and `width` across it.
    // Everything else in the cluster sits square to the panel; this is here for
    // the one icon that doesn't.
    void AppendOrientedQuad(std::vector<Vertex>& out, float x, float y, float dx, float dy,
                            float length, float width, const XMFLOAT4& c)
    {
        const float px = -dy * width * 0.5f, py = dx * width * 0.5f;
        const float ex = x + dx * length, ey = y + dy * length;
        AppendTriangle(out, x - px, y - py, x + px, y + py, ex + px, ey + py, c);
        AppendTriangle(out, x - px, y - py, ex + px, ey + py, ex - px, ey - py, c);
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

    // A combat knife on the diagonal: a tapered blade up to the point at the
    // top right, a crossguard over its neck, and the grip below. The angle is
    // what earns it its own silhouette — stood upright, a blade and the
    // cartridge two modules along are the same pointed shape at this size.
    void DrawMeleeIcon(std::vector<Vertex>& out, float x, float y, float size, const XMFLOAT4& c)
    {
        constexpr float kDx = 0.7071f, kDy = -0.7071f; // blade axis: up and right
        const float gx = x + size * 0.34f, gy = y + size * 0.66f; // the guard
        const float halfW = size * 0.10f;

        // Grip, running back down the axis from the guard.
        AppendOrientedQuad(out, gx, gy, -kDx, -kDy, size * 0.30f, size * 0.15f, Shade(c, 0.58f));
        // Blade: a spike off the guard, full width at the base and nothing at
        // the point.
        const float px = -kDy * halfW, py = kDx * halfW;
        AppendTriangle(out, gx - px, gy - py, gx + px, gy + py, gx + kDx * size * 0.68f,
                       gy + kDy * size * 0.68f, c);
        // Crossguard, laid across the neck on top of both.
        AppendOrientedQuad(out, gx + kDy * size * 0.20f, gy - kDx * size * 0.20f, -kDy, kDx,
                           size * 0.40f, size * 0.09f, Shade(c, 0.78f));
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

    // A syringe stood upright: plunger and thumb pad on top, barrel, then the
    // needle out the bottom. The rod above the body is what keeps it apart from
    // the cartridge two modules back — both are tall and narrow, but only one
    // of them has anything sticking out of the top.
    //
    // It's the medic's dressing drawn literally, which is fine while the medic
    // owns the only ability in the game. The second one will want this switched
    // on the ability's kind, the way the module's numbers already are.
    void DrawAbilityIcon(std::vector<Vertex>& out, float x, float y, float size, const XMFLOAT4& c)
    {
        // Plunger: thumb pad across the top and the rod running down from it.
        AppendQuad(out, x + size * 0.31f, y, size * 0.38f, size * 0.07f, Shade(c, 0.62f));
        AppendQuad(out, x + size * 0.44f, y + size * 0.07f, size * 0.12f, size * 0.17f,
                   Shade(c, 0.62f));
        // Barrel, with its finger flange at the shoulder.
        AppendQuad(out, x + size * 0.24f, y + size * 0.22f, size * 0.52f, size * 0.06f,
                   Shade(c, 0.78f));
        AppendRoundRect(out, x + size * 0.33f, y + size * 0.24f, size * 0.34f, size * 0.48f,
                        size * 0.06f, c);
        // Hub and needle.
        AppendQuad(out, x + size * 0.42f, y + size * 0.70f, size * 0.16f, size * 0.08f,
                   Shade(c, 0.78f));
        AppendQuad(out, x + size * 0.47f, y + size * 0.76f, size * 0.06f, size * 0.24f,
                   Shade(c, 0.90f));
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
        Melee,
        Grenade,
        Ability,
        Contact,
    };

    void DrawIcon(std::vector<Vertex>& out, Icon icon, float x, float y, float size,
                  const XMFLOAT4& c)
    {
        switch (icon)
        {
        case Icon::Health: DrawHealthIcon(out, x, y, size, c); break;
        case Icon::Ammo: DrawAmmoIcon(out, x, y, size, c); break;
        case Icon::Melee: DrawMeleeIcon(out, x, y, size, c); break;
        case Icon::Grenade: DrawGrenadeIcon(out, x, y, size, c); break;
        case Icon::Ability: DrawAbilityIcon(out, x, y, size, c); break;
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

    // As many key caps as the row will take. There are seven today; the ceiling
    // is here so the layout has a fixed amount of stack to work in, not because
    // the number means anything.
    constexpr size_t kMaxHints = 10;

    // A countdown as minutes and seconds. Rounded up, so it reaches 0:01 for
    // the last second and only says 0:00 when the match really is over.
    std::string ClockText(float seconds)
    {
        const int left = std::max(0, static_cast<int>(std::ceil(seconds)));
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d:%02d", left / 60, left % 60);
        return buf;
    }
}

XMFLOAT4 Hud::HealthColor(float fraction)
{
    return fraction > 0.5f ? kHealthGood : (fraction > 0.25f ? kHealthWarn : kHealthLow);
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
    const XMFLOAT4 hpColor = HealthColor(hpFrac);

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

    // The blade's charges get the pip row a small magazine gets, and the same
    // sweep while they're coming back: it is a magazine, whatever it holds, and
    // reading it should cost the same glance. It sits next to the ammo module
    // because that's the pairing that matters — when one of them is empty, the
    // other is the whole answer to what the player can still do.
    const bool recovering = st.meleeRecoverFraction >= 0.0f;

    Module melee;
    melee.icon = Icon::Melee;
    melee.iconColor = recovering ? kReloadColor : kMeleeColor;
    melee.value = std::to_string(st.melee);
    melee.valueColor = recovering ? kMutedColor : kValueColor;
    melee.barColor = recovering ? kReloadColor : kMeleeColor;
    if (recovering)
    {
        melee.bar = Bar::Fill;
        melee.fill = std::clamp(st.meleeRecoverFraction, 0.0f, 1.0f);
    }
    else if (st.meleeCharges > 0)
    {
        melee.bar = Bar::Pips;
        melee.pips = st.meleeCharges;
        melee.pipsFull = std::clamp(st.melee, 0, st.meleeCharges);
    }

    // Spent equipment greys out rather than disappearing: the slot is still
    // part of the loadout, it just has nothing in it until the next life.
    Module grenade;
    grenade.icon = Icon::Grenade;
    grenade.iconColor = st.grenades > 0 ? kGrenadeColor : kSpentColor;
    grenade.value = std::to_string(st.grenades);
    grenade.valueColor = st.grenades > 0 ? kValueColor : kMutedColor;

    // The class ability, last in the row: everything to its left is what every
    // soldier carries, so the one thing that isn't shared sits at the end of
    // the loadout rather than inside it.
    //
    // Three states, one module. Ready says so in a word — a number would be a
    // zero, and a zero next to an ammo count reads as empty. Running counts the
    // seconds of it left, since what the player is deciding is whether to see
    // it out. Recharging counts the seconds until it's back, in the reload's
    // color, because that's the same wait the rest of the panel already draws.
    Module abilityModule;
    if (st.ability)
    {
        const bool running = st.abilityFraction >= 0.0f;
        const bool recharging = !running && st.abilityCooldown > 0.0f;
        const float left =
            running ? (1.0f - st.abilityFraction) * st.ability->duration : st.abilityCooldown;

        abilityModule.icon = Icon::Ability;
        abilityModule.iconColor = recharging ? kReloadColor : kAbilityColor;
        abilityModule.value = running || recharging
                                  ? std::to_string(static_cast<int>(std::ceil(left)))
                                  : "RDY";
        abilityModule.valueColor = recharging ? kMutedColor : kValueColor;
        abilityModule.bar = Bar::Fill;
        abilityModule.barColor = recharging ? kReloadColor : kAbilityColor;
        abilityModule.fill =
            running      ? std::clamp(st.abilityFraction, 0.0f, 1.0f)
            : recharging ? std::clamp(1.0f - st.abilityCooldown / st.ability->cooldown, 0.0f, 1.0f)
                         : 1.0f;
    }

    Module modules[5];
    size_t moduleCount = 0;
    const auto add = [&](const Module& m) { modules[moduleCount++] = m; };
    add(health);
    add(ammo);
    add(melee);
    add(grenade);
    if (st.ability)
        add(abilityModule);

    // --- Layout ---

    const float iconSize = kIconSize * s;
    const float valueSize = kValueSize * s;
    const float pad = kPanelPad * s;

    float contentW = 0.0f;
    for (size_t i = 0; i < moduleCount; ++i)
    {
        Module& m = modules[i];
        const float textW = renderer.MeasureScreenText(m.value, valueSize);
        m.width = std::max(m.minWidth * s, iconSize + kIconGap * s + textW);
        contentW += m.width;
    }
    contentW += kModuleGap * s * (moduleCount - 1);

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

    for (size_t i = 0; i < moduleCount; ++i)
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
    // of text: centered with everything else, and short enough to stop
    // competing with the readouts for the eye. Which bindings are worth the
    // space is settled before they get here (Hud::State::hints); this end knows
    // only how to lay a row of caps out, and stops at kMaxHints of them so a
    // caller can't quietly widen the row past the screen.

    const float hintSize = kHintSize * s;
    const float capH = hintSize + kHintKeyPad * s * 2.0f;
    const float hintY = panelY - kHintGap * s - capH;

    const size_t hintCount = std::min(st.hintCount, kMaxHints);
    float hintsW = 0.0f;
    float capWidths[kMaxHints] = {};
    for (size_t i = 0; i < hintCount; ++i)
    {
        capWidths[i] = std::max(renderer.MeasureScreenText(st.hints[i].key, hintSize) +
                                    kHintKeyPad * s * 2.0f,
                                capH);
        hintsW += capWidths[i] + kHintTextGap * s +
                  renderer.MeasureScreenText(st.hints[i].label, hintSize);
    }
    if (hintCount > 0)
        hintsW += kHintSpacing * s * (hintCount - 1);

    float hintX = std::floor((w - hintsW) * 0.5f);
    for (size_t i = 0; i < hintCount; ++i)
    {
        const Hint& hint = st.hints[i];
        AppendRoundRect(tris, hintX, hintY, capWidths[i], capH, capH * 0.28f,
                        Fade(kTrack, fade));
        renderer.DrawScreenText(hint.key,
                                hintX + (capWidths[i] -
                                         renderer.MeasureScreenText(hint.key, hintSize)) *
                                            0.5f,
                                hintY + kHintKeyPad * s, hintSize, Fade(kValueColor, fade));

        hintX += capWidths[i] + kHintTextGap * s;
        renderer.DrawScreenText(hint.label, hintX, hintY + (capH - hintSize) * 0.5f, hintSize,
                                Fade(kMutedColor, fade));
        hintX += renderer.MeasureScreenText(hint.label, hintSize) + kHintSpacing * s;
    }

    // --- Roster ---
    //
    // Top right: the match clock, then two rows, your side over theirs, each a
    // soldier icon in that side's color, how many of it are standing, a pip per
    // slot on the roster, and what that side has killed. The pips are what make
    // the strength readable without reading — five lit is a squad, two lit and
    // three dark is a squad in trouble — and they're the same pips the magazine
    // and the blade already use, because "how many of a fixed number are left"
    // is the same question in all three places.
    //
    // The clock and the kills are in here rather than in a panel of their own
    // because they're the same glance as the strength: who is winning, by how
    // much, and how long there is left to do anything about it. The kills sit
    // at the right edge in their side's own color, away from the standing count
    // and apart from it — the two numbers mean very different things, and a
    // reader who mixed them up would misread the match rather than misread a
    // panel.
    //
    // Deliberately not faded during the respawn wait, unlike everything above.
    // The loadout dims because it's describing kit nobody is holding; the
    // roster is describing a fight that is still going on without the player,
    // and the wait is exactly when they have nothing to do but read it.

    const float rosterIcon = kRosterIconSize * s;
    const float rosterValue = kRosterValueSize * s;
    const float rosterGap = kRosterGap * s;
    const float pipsW = kRosterPipsWidth * s;
    const float rosterScore = kRosterScoreSize * s;
    // The row is as tall as the tallest thing standing in it, which is now the
    // kill count rather than the soldier icon.
    const float rowH = std::max({ rosterIcon, barH, rosterScore });

    struct Side
    {
        int up;
        int score;
        XMFLOAT4 color;
    };
    const Side sides[2] = { { st.allies, st.allyScore, Lift(st.allyColor, kSideLift) },
                            { st.enemies, st.enemyScore, Lift(st.enemyColor, kSideLift) } };

    // One count column wide enough for either row, so the two pip strips line
    // up under each other. A roster whose bars didn't share an edge would be a
    // roster you had to read twice to compare.
    std::string counts[2];
    std::string scores[2];
    float countW = 0.0f;
    float scoreW = 0.0f;
    for (int i = 0; i < 2; ++i)
    {
        counts[i] = std::to_string(std::max(sides[i].up, 0));
        countW = std::max(countW, renderer.MeasureScreenText(counts[i], rosterValue));
        scores[i] = std::to_string(std::max(sides[i].score, 0));
        scoreW = std::max(scoreW, renderer.MeasureScreenText(scores[i], rosterScore));
    }

    const std::string clockText = ClockText(st.clock);
    const float clockSize = kRosterClockSize * s;
    const float clockW = renderer.MeasureScreenText(clockText, clockSize);

    const bool drawPips = st.teamSize > 0 && st.teamSize <= kMaxPips;
    const float rosterContentW = rosterIcon + rosterGap + countW +
                                 (drawPips ? rosterGap + pipsW : 0.0f) + rosterGap + scoreW;
    // The clock is centered over the rows and can't be allowed to spill out of
    // the panel it's centered in, however wide the digits are.
    const float rosterW = std::max(rosterContentW, clockW) + pad * 2.0f;
    const float rosterH =
        clockSize + kRosterHeadGap * s + rowH * 2.0f + kRosterRowGap * s + pad * 2.0f;
    const float rosterX = std::floor(w - kSideMargin * s - rosterW);
    const float rosterY = std::floor(kTopMargin * s);

    AppendRoundRect(tris, rosterX, rosterY, rosterW, rosterH, kPanelCorner * s, kPanelBg);

    // The clock, centered above the two sides — it belongs to neither of them,
    // and it's the one thing in the panel that is the same for both. A rule
    // under it separates the match from the sides playing it; the rows below
    // get no such line, because those two are a comparison and cutting them
    // apart would be working against the reading.
    renderer.DrawScreenText(clockText, rosterX + (rosterW - clockW) * 0.5f, rosterY + pad,
                            clockSize,
                            st.matchOver ? kMutedColor
                            : st.clock <= kClockWarning ? kReloadColor
                                                        : kValueColor);
    const float rowsTop = rosterY + pad + clockSize + kRosterHeadGap * s;
    AppendQuad(tris, rosterX + pad, rowsTop - kRosterHeadGap * s * 0.5f, rosterW - pad * 2.0f,
               std::max(1.0f * s, 1.0f), kDivider);

    for (int i = 0; i < 2; ++i)
    {
        const Side& side = sides[i];
        const float rowTop = rowsTop + static_cast<float>(i) * (rowH + kRosterRowGap * s);
        float rx = rosterX + pad;

        // No rule between the rows, unlike the modules below. Those are six
        // unrelated readouts that would run together without one; these are two
        // halves of a comparison, already told apart by color, and a line drawn
        // faintly enough not to cut the panel in two is a line nobody can see.

        // A side wiped out greys out rather than vanishing, the same way a
        // spent grenade does: the slots are still on the roster, there's just
        // nobody standing in them this second.
        const XMFLOAT4 iconColor = side.up > 0 ? side.color : kSpentColor;
        DrawIcon(tris, Icon::Contact, rx, rowTop + (rowH - rosterIcon) * 0.5f, rosterIcon,
                 iconColor);
        rx += rosterIcon + rosterGap;

        renderer.DrawScreenText(counts[i], rx, rowTop + (rowH - rosterValue) * 0.5f, rosterValue,
                                side.up > 0 ? kValueColor : kMutedColor);
        rx += countW + rosterGap;

        // The kills, right-aligned against the panel's edge rather than laid
        // out from the left like everything else in the row: two numbers being
        // compared want to share their last digit's column, not their first's.
        renderer.DrawScreenText(
            scores[i],
            rosterX + rosterW - pad - renderer.MeasureScreenText(scores[i], rosterScore),
            rowTop + (rowH - rosterScore) * 0.5f, rosterScore, side.color);

        if (!drawPips)
            continue;

        const float pipW = (pipsW - kPipGap * s * (st.teamSize - 1)) / st.teamSize;
        const float pipY = rowTop + (rowH - barH) * 0.5f;
        for (int p = 0; p < st.teamSize; ++p)
            AppendRoundRect(tris, rx + p * (pipW + kPipGap * s), pipY, pipW, barH, barH * 0.35f,
                            p < side.up ? side.color : kTrack);
    }

    renderer.DrawScreenTriangles(tris.data(), static_cast<uint32_t>(tris.size()));
}
