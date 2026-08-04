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

    // The scoreboard, held up over the middle of the screen. Authored at the
    // same 1080 scale as everything else, and deliberately the largest thing
    // the HUD draws: nothing is happening while it's up — the player is reading
    // rather than fighting — so it can afford the room the corner panel can't.
    constexpr float kBoardWidth = 900.0f;
    constexpr float kBoardPad = 26.0f;
    constexpr float kBoardTitleSize = 30.0f;
    constexpr float kBoardClockSize = 22.0f;
    constexpr float kBoardHeadSize = 24.0f; // a side's name over its column
    constexpr float kBoardLabelSize = 14.0f; // the K / D column captions
    constexpr float kBoardRowSize = 20.0f;
    constexpr float kBoardRowHeight = 30.0f;
    constexpr float kBoardColumnGap = 34.0f;
    constexpr float kBoardHeadGap = 16.0f;
    constexpr float kBoardTitleGap = 20.0f;
    // What the two number columns are given, and what's left over goes to the
    // name. Fixed rather than measured so both sides' digits line up in the
    // same place whichever column they're in.
    constexpr float kBoardNumberWidth = 62.0f;
    constexpr XMFLOAT4 kBoardBg = { 0.025f, 0.035f, 0.055f, 0.92f };
    constexpr XMFLOAT4 kBoardRowBg = { 1.00f, 1.00f, 1.00f, 0.035f };
    // The player's own row, lifted out of the list. Brighter than a row and
    // dimmer than a side's color, so it reads as "this one is you" without
    // competing with which side you're on.
    constexpr XMFLOAT4 kBoardYouBg = { 1.00f, 1.00f, 1.00f, 0.10f };

    // The radar, bottom left. The box the arena is fitted inside rather than
    // the size it comes out: the map decides the panel's shape, and these two
    // numbers only say how much of the screen it may take. Near enough square
    // because the map arrives turned to face the camera and a long letterbox
    // laid on a diagonal needs the same room either way — see Hud.h. Big
    // enough that hardcorps2t's 197 units land at better than a pixel each,
    // which is what makes a trench mouth a shape rather than a smudge.
    constexpr float kRadarMaxWidth = 264.0f;
    constexpr float kRadarMaxHeight = 264.0f;
    // A soldier is 0.8 units across and would be a single pixel drawn to
    // scale, so the blips are exaggerated to about six units wide. That's the
    // trade every radar makes: positions stay true, sizes stop being. Ten of
    // them on a panel this size is a readable scatter rather than a clump.
    constexpr float kBlipRadius = 5.0f;
    constexpr float kBlipCore = 2.4f; // the class mark inside it
    constexpr float kBlipArrow = 10.0f; // how far the player's heading reaches
    constexpr XMFLOAT4 kRadarField = { 0.00f, 0.00f, 0.00f, 0.30f };
    // The rock the middle is navigated around, in the slate the arena floor is
    // already drawn in. Dark enough to stay underneath the blips — terrain on
    // this panel is context, and the moment it competes with a body it has
    // stopped being context.
    constexpr XMFLOAT4 kRadarWall = { 0.42f, 0.47f, 0.56f, 0.55f };

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

    // The most points a shape handed to AppendClipped may have. The largest
    // today is a blip's disc at kDiscSegments; the room past it is what four
    // clipping edges can add.
    constexpr size_t kClipMaxPoints = 32;
    constexpr int kDiscSegments = 20;

    // A convex polygon trimmed to a rectangle and fanned into triangles.
    //
    // Only the radar needs this, and only since the box stopped holding the
    // whole arena: zoomed in, the map is bigger than the panel it's drawn in
    // and everything on it — ground, rock, bodies — runs off the edge. The
    // alternative was a scissor rect, which would mean the renderer's screen
    // pass keeping one per batch; this keeps the panel's geometry the panel's
    // business, which is how the rest of this file already works.
    //
    // Sutherland-Hodgman, four half-planes, no allocation: a convex polygon
    // clipped to a rectangle stays convex and gains at most one point per edge,
    // so the working buffers are fixed and a fan from the first point is a
    // correct triangulation of the result. A shape entirely outside vanishes
    // partway through and returns nothing, which is the cheap path for a body
    // standing off the edge of what the box currently holds.
    void AppendClipped(std::vector<Vertex>& out, const XMFLOAT2* pts, size_t count, float minX,
                       float minY, float maxX, float maxY, const XMFLOAT4& color)
    {
        if (count < 3 || count > kClipMaxPoints)
            return;

        XMFLOAT2 buf[2][kClipMaxPoints + 4];
        size_t n = count;
        for (size_t i = 0; i < count; ++i)
            buf[0][i] = pts[i];

        int cur = 0;
        for (int edge = 0; edge < 4; ++edge)
        {
            const XMFLOAT2* in = buf[cur];
            XMFLOAT2* dst = buf[1 - cur];
            const auto inside = [&](const XMFLOAT2& p) {
                switch (edge)
                {
                case 0:  return p.x >= minX;
                case 1:  return p.x <= maxX;
                case 2:  return p.y >= minY;
                default: return p.y <= maxY;
                }
            };
            // Only ever called for a pair straddling the edge, so the axis the
            // fraction is taken along is guaranteed to span it.
            const auto meet = [&](const XMFLOAT2& a, const XMFLOAT2& b) {
                float t = 0.0f;
                switch (edge)
                {
                case 0:  t = (minX - a.x) / (b.x - a.x); break;
                case 1:  t = (maxX - a.x) / (b.x - a.x); break;
                case 2:  t = (minY - a.y) / (b.y - a.y); break;
                default: t = (maxY - a.y) / (b.y - a.y); break;
                }
                return XMFLOAT2{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            };

            size_t m = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const XMFLOAT2& a = in[i];
                const XMFLOAT2& b = in[(i + 1) % n];
                const bool aIn = inside(a), bIn = inside(b);
                if (aIn)
                    dst[m++] = a;
                if (aIn != bIn)
                    dst[m++] = meet(a, b);
            }
            n = m;
            cur = 1 - cur;
            if (n < 3)
                return;
        }

        for (size_t i = 1; i + 1 < n; ++i)
            AppendTriangle(out, buf[cur][0].x, buf[cur][0].y, buf[cur][i].x, buf[cur][i].y,
                           buf[cur][i + 1].x, buf[cur][i + 1].y, color);
    }

    void AppendClippedDisc(std::vector<Vertex>& out, float cx, float cy, float radius, float minX,
                           float minY, float maxX, float maxY, const XMFLOAT4& color)
    {
        XMFLOAT2 pts[kDiscSegments];
        for (int i = 0; i < kDiscSegments; ++i)
        {
            const float a = XM_2PI * static_cast<float>(i) / kDiscSegments;
            pts[i] = { cx + std::cos(a) * radius, cy + std::sin(a) * radius };
        }
        AppendClipped(out, pts, kDiscSegments, minX, minY, maxX, maxY, color);
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

    // --- Key caps ---
    //
    // A row of "this control does this": a rounded cap with whatever the player
    // has the control bound to, then what it does, muted, and on to the next.
    //
    // Two readouts draw one now — the loadout's row under the cluster, and the
    // radar's pair over the corner — so the rule for laying one out lives here
    // and each caller says only where its row starts. Splitting the measure
    // from the draw is what lets a caller center its row (the loadout) or
    // butt it up against the edge of its own panel (the radar) without either
    // of them knowing how a cap is built.
    //
    // The cap is at least as wide as it is tall, so a one-character key stays a
    // square rather than becoming a slot.
    float CapWidth(Renderer& renderer, const char* key, float size, float s)
    {
        const float capH = size + kHintKeyPad * s * 2.0f;
        return std::max(renderer.MeasureScreenText(key, size) + kHintKeyPad * s * 2.0f, capH);
    }

    float HintsWidth(Renderer& renderer, const Hud::Hint* hints, size_t count, float s)
    {
        if (count == 0)
            return 0.0f;
        const float size = kHintSize * s;
        float width = kHintSpacing * s * static_cast<float>(count - 1);
        for (size_t i = 0; i < count; ++i)
            width += CapWidth(renderer, hints[i].key, size, s) + kHintTextGap * s +
                     renderer.MeasureScreenText(hints[i].label, size);
        return width;
    }

    // Draws the row with its top-left at (x, y); the height is CapHeight below.
    void DrawHints(Renderer& renderer, std::vector<Vertex>& tris, const Hud::Hint* hints,
                   size_t count, float x, float y, float s, float fade)
    {
        const float size = kHintSize * s;
        const float capH = size + kHintKeyPad * s * 2.0f;
        for (size_t i = 0; i < count; ++i)
        {
            const float capW = CapWidth(renderer, hints[i].key, size, s);
            AppendRoundRect(tris, x, y, capW, capH, capH * 0.28f, Fade(kTrack, fade));
            renderer.DrawScreenText(
                hints[i].key,
                x + (capW - renderer.MeasureScreenText(hints[i].key, size)) * 0.5f,
                y + kHintKeyPad * s, size, Fade(kValueColor, fade));

            x += capW + kHintTextGap * s;
            renderer.DrawScreenText(hints[i].label, x, y + (capH - size) * 0.5f, size,
                                    Fade(kMutedColor, fade));
            x += renderer.MeasureScreenText(hints[i].label, size) + kHintSpacing * s;
        }
    }

    float CapHeight(float s) { return kHintSize * s + kHintKeyPad * s * 2.0f; }

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

    const size_t hintCount = std::min(st.hintCount, kMaxHints);
    const float hintsW = HintsWidth(renderer, st.hints, hintCount, s);
    DrawHints(renderer, tris, st.hints, hintCount, std::floor((w - hintsW) * 0.5f),
              panelY - kHintGap * s - CapHeight(s), s, fade);

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

void Hud::RenderScoreboard(Renderer& renderer, const Scoreboard& board)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());

    // Split the roster into its two columns and put each in the order a
    // scoreboard is read in: most kills first, then fewest deaths, and on a
    // dead heat the people ahead of the bots and the present ahead of the
    // departed. That last pair of rules is the only part that isn't about
    // performance — the players in the match are who somebody opening this is
    // looking for, and burying them among bots with identical lines is how a
    // board becomes something to search rather than something to read. A leaver
    // who topped the column still sits at the top of it, though: what they did
    // is why the total says what it says.
    const auto rank = [](const ScoreRow* row) {
        switch (row->holder)
        {
        case Holder::Human: return 0;
        case Holder::Ai:    return 1;
        default:            return 2; // Left
        }
    };
    std::vector<const ScoreRow*> columns[2];
    for (int side = 0; side < 2; ++side)
    {
        for (size_t i = 0; i < board.rowCount; ++i)
            if (board.rows[i].team == board.teams[side])
                columns[side].push_back(&board.rows[i]);
        std::stable_sort(columns[side].begin(), columns[side].end(),
                         [&](const ScoreRow* a, const ScoreRow* b) {
                             if (a->kills != b->kills)
                                 return a->kills > b->kills;
                             if (a->deaths != b->deaths)
                                 return a->deaths < b->deaths;
                             return rank(a) < rank(b);
                         });
    }

    const size_t rowCount = std::max(columns[0].size(), columns[1].size());

    // The board is authored at the same 1080 scale as the rest of the HUD, but
    // it's the one panel whose height is decided by its contents: every player
    // who leaves adds a row that stays for the match. So it shrinks to fit
    // rather than running off the screen — everything in proportion, because a
    // board that dropped rows to make room would be a board that stopped adding
    // up, which is the one thing it can't do.
    const float designH = kBoardPad * 2.0f + kBoardTitleSize + kBoardTitleGap + kBoardHeadSize +
                          kBoardHeadGap + kBoardLabelSize +
                          kBoardRowHeight * static_cast<float>(rowCount);
    float s = h / kDesignHeight;
    s = std::min(s, (h - kTopMargin * s * 2.0f) / designH);

    const float pad = kBoardPad * s;
    const float rowH = kBoardRowHeight * s;
    const float titleSize = kBoardTitleSize * s;
    const float clockSize = kBoardClockSize * s;
    const float headSize = kBoardHeadSize * s;
    const float labelSize = kBoardLabelSize * s;
    const float rowSize = kBoardRowSize * s;
    const float numberW = kBoardNumberWidth * s;

    const float boardW = std::min(kBoardWidth * s, w - kSideMargin * s * 2.0f);
    const float columnW = (boardW - pad * 2.0f - kBoardColumnGap * s) * 0.5f;
    const float boardH = pad * 2.0f + titleSize + kBoardTitleGap * s + headSize +
                         kBoardHeadGap * s + labelSize + rowH * static_cast<float>(rowCount);
    const float boardX = std::floor((w - boardW) * 0.5f);
    const float boardY = std::floor((h - boardH) * 0.5f);

    std::vector<Vertex> tris;
    AppendRoundRect(tris, boardX, boardY, boardW, boardH, kPanelCorner * s * 1.5f, kBoardBg);

    // The heading: what the board is, and how long there is left of the thing
    // it's counting. Once the match is over the clock belongs to the result
    // screen behind this panel, so the word takes its place — a board still
    // showing a countdown would be counting something that has already
    // finished.
    const char* title = board.matchOver ? "FINAL" : "SCOREBOARD";
    renderer.DrawScreenText(title, boardX + pad, boardY + pad, titleSize, kValueColor);
    if (!board.matchOver)
    {
        const std::string clock = ClockText(board.clock);
        renderer.DrawScreenText(clock,
                                boardX + boardW - pad -
                                    renderer.MeasureScreenText(clock, clockSize),
                                boardY + pad + (titleSize - clockSize) * 0.5f, clockSize,
                                board.clock <= kClockWarning ? kReloadColor : kMutedColor);
    }

    const float headY = boardY + pad + titleSize + kBoardTitleGap * s;
    const float labelY = headY + headSize + kBoardHeadGap * s;
    const float rowsY = labelY + labelSize;

    for (int side = 0; side < 2; ++side)
    {
        const float colX = boardX + pad + static_cast<float>(side) * (columnW + kBoardColumnGap * s);
        const XMFLOAT4 color = Lift(board.teamColors[side], kSideLift);
        // Where each column's numbers end: deaths against the column's right
        // edge, kills a fixed step in from it. Right-aligned, because two
        // numbers being compared down a list want to share their last digit's
        // column and not their first's — the same rule the corner panel's
        // scores follow.
        const float killsRight = colX + columnW - numberW;
        const float deathsRight = colX + columnW;

        // The side's name and what it has killed, over its own column, in its
        // own color. The total is the loud number here: it's the one the match
        // is decided on, and the rows underneath are the working.
        renderer.DrawScreenText(board.teamNames[side], colX, headY, headSize, color);
        const std::string total = std::to_string(std::max(board.teamScores[side], 0));
        renderer.DrawScreenText(total,
                                deathsRight - renderer.MeasureScreenText(total, headSize), headY,
                                headSize, color);
        AppendQuad(tris, colX, headY + headSize + kBoardHeadGap * s * 0.45f, columnW,
                   std::max(1.0f * s, 1.0f), kDivider);

        // The column captions. Single letters because the numbers under them
        // are what's being read and a word would outweigh them.
        const auto caption = [&](const char* text, float right) {
            renderer.DrawScreenText(text, right - renderer.MeasureScreenText(text, labelSize),
                                    labelY, labelSize, kMutedColor);
        };
        caption("K", killsRight);
        caption("D", deathsRight);

        for (size_t i = 0; i < columns[side].size(); ++i)
        {
            const ScoreRow& row = *columns[side][i];
            const float rowY = rowsY + static_cast<float>(i) * rowH;
            const float textY = rowY + (rowH - rowSize) * 0.5f;

            // Banded rows, with the player's own lifted clear of the pattern.
            if (row.you)
                AppendRoundRect(tris, colX - pad * 0.35f, rowY, columnW + pad * 0.7f, rowH,
                                rowH * 0.25f, kBoardYouBg);
            else if (i % 2 == 1)
                AppendQuad(tris, colX - pad * 0.35f, rowY, columnW + pad * 0.7f, rowH, kBoardRowBg);

            // Three brightnesses, loudest first: a player present is drawn in
            // their side's own color, the AI in the panel's muted grey, and a
            // player who has left in the darker grey everything spent is drawn
            // in — a magazine's fired rounds, a wiped-out side's icon. Their
            // score stays on the board because it's part of the total above it,
            // but they aren't in the fight any more and shouldn't read as
            // though they are. Which of the ten are people is the first thing
            // anybody wants off this board, and color carries it better than a
            // marker glyph next to a name would.
            const XMFLOAT4 present = row.you ? kValueColor : color;
            const XMFLOAT4 nameColor = row.holder == Holder::Left  ? kSpentColor
                                       : row.holder == Holder::Ai ? kMutedColor
                                                                  : present;
            // A place nothing has stood in yet: the five seconds between a side
            // being owed a soldier and getting one. Said with a dash rather
            // than a blank, so the row reads as empty rather than as broken.
            renderer.DrawScreenText(row.name ? row.name : "-", colX, textY, rowSize, nameColor);

            const XMFLOAT4 numberColor = row.holder == Holder::Left ? kSpentColor
                                         : row.holder == Holder::Human ? kValueColor
                                                                       : kMutedColor;
            const auto number = [&](int value, float right) {
                const std::string text = std::to_string(std::max(value, 0));
                renderer.DrawScreenText(text, right - renderer.MeasureScreenText(text, rowSize),
                                        textY, rowSize, numberColor);
            };
            number(row.kills, killsRight);
            number(row.deaths, deathsRight);
        }
    }

    renderer.DrawScreenTriangles(tris.data(), static_cast<uint32_t>(tris.size()));
}

void Hud::RenderRadar(Renderer& renderer, const Radar& radar)
{
    const float h = static_cast<float>(renderer.Height());
    const float s = h / kDesignHeight;
    const float pad = kPanelPad * s;

    const float halfX = std::max(radar.arenaHalf.x, 0.001f);
    const float halfZ = std::max(radar.arenaHalf.y, 0.001f);

    // The turn that makes the panel agree with the screen. `right` is the
    // ground direction that runs rightward across the view; up is the same
    // vector a quarter turn on, negated on the way out because screen y runs
    // down. Normalized rather than trusted — an unset camera would otherwise
    // collapse the whole map to a point.
    float rx = radar.screenRight.x, rz = radar.screenRight.y;
    const float rlen = std::sqrt(rx * rx + rz * rz);
    if (rlen > 1e-4f)
    {
        rx /= rlen;
        rz /= rlen;
    }
    else
    {
        rx = 1.0f;
        rz = 0.0f;
    }
    const auto toU = [&](float x, float z) { return x * rx + z * rz; };
    const auto toV = [&](float x, float z) { return x * rz - z * rx; };

    // How much room the turned arena needs. The corners are the extremes of
    // both axes, and the arena is centered on the origin, so each half-extent
    // is just the two contributions added with their signs thrown away.
    const float halfU = std::fabs(rx) * halfX + std::fabs(rz) * halfZ;
    const float halfV = std::fabs(rz) * halfX + std::fabs(rx) * halfZ;

    // The box: one scale for both axes, picked so the whole arena fits inside
    // it and touches one edge. The panel shrinks to the map rather than the map
    // stretching to the panel — which is what keeps a bearing read off this
    // thing the same bearing the player would walk — and having been sized
    // once it never moves again. Zoom changes what the box holds, not what the
    // box costs: a readout that grew and shrank in the corner of the screen
    // would be a readout the eye had to find each time.
    const float fit = std::min(kRadarMaxWidth * s / (halfU * 2.0f),
                               kRadarMaxHeight * s / (halfV * 2.0f));
    const float fieldW = halfU * 2.0f * fit;
    const float fieldH = halfV * 2.0f * fit;

    const float panelW = fieldW + pad * 2.0f;
    const float panelH = fieldH + pad * 2.0f;
    const float panelX = std::floor(kSideMargin * s);
    const float panelY = std::floor(h - kBottomMargin * s - panelH);
    const float fieldX = panelX + pad;
    const float fieldY = panelY + pad;
    const float fieldRight = fieldX + fieldW;
    const float fieldBottom = fieldY + fieldH;

    const float scale = fit * std::max(radar.zoom, 1.0f);

    // Where the window looks. Half the box, measured in the arena's own units,
    // is how much map is on show either side of the middle; the focus is pushed
    // in from the edges by exactly that, so the ground never scrolls off and
    // leaves the panel half empty. An axis with more box than map stays
    // centered, which is what makes zoom 1 the picture it always was.
    const float viewU = fieldW * 0.5f / scale;
    const float viewV = fieldH * 0.5f / scale;
    const auto middle = [](float want, float half, float limit) {
        return half >= limit ? 0.0f : std::clamp(want, -limit + half, limit - half);
    };
    const float centerU = middle(toU(radar.focusX, radar.focusZ), viewU, halfU);
    const float centerV = middle(toV(radar.focusX, radar.focusZ), viewV, halfV);

    // A spot on the ground to a spot on the panel: turned to face the camera,
    // then scaled and slid so the window's middle lands in the box's middle.
    const auto map = [&](float x, float z) {
        return XMFLOAT2{ fieldX + fieldW * 0.5f + (toU(x, z) - centerU) * scale,
                         fieldY + fieldH * 0.5f + (toV(x, z) - centerV) * scale };
    };
    static std::vector<Vertex> tris;
    tris.clear();

    // Everything inside the box goes through here. Zoomed in there is more map
    // than panel, so a shape that used to be guaranteed to land inside its own
    // frame no longer is — ground, rock and bodies alike run off the edge, and
    // the edge is the only thing that says where the readout stops.
    const auto clipped = [&](const XMFLOAT2* pts, size_t n, const XMFLOAT4& color) {
        AppendClipped(tris, pts, n, fieldX, fieldY, fieldRight, fieldBottom, color);
    };

    AppendRoundRect(tris, panelX, panelY, panelW, panelH, kPanelCorner * s, kPanelBg);

    // The ground itself, darker than the panel around it. On a map turned to
    // face the camera this is doing real work rather than tidying an edge: the
    // arena is a diamond, so at low zoom there would otherwise be nothing to
    // say which of the box is ground and which is margin. At high zoom the
    // diamond covers the box entirely, which is the whole point of being able
    // to zoom at all.
    const XMFLOAT2 corners[4] = { map(-halfX, -halfZ), map(halfX, -halfZ), map(halfX, halfZ),
                                  map(-halfX, halfZ) };
    clipped(corners, 4, kRadarField);

    // Terrain next, so nothing about the map can end up drawn over a soldier.
    // A rock thinner than a pixel is still worth a pixel: the outcrops are read
    // as a belt rather than one at a time, and a belt with gaps punched in it
    // by rounding is a different belt. Widening happens in the arena's units,
    // before the turn, because after it there's no axis left to widen along —
    // and it costs less the further in the player zooms, which is correct: the
    // rock is only being fattened to survive the drawing.
    const float minWorld = scale > 0.0f ? 1.0f / scale : 0.0f;
    for (size_t i = 0; i < radar.wallCount; ++i)
    {
        const Visibility::Rect& r = radar.walls[i];
        const float padX = std::max(minWorld - (r.maxX - r.minX), 0.0f) * 0.5f;
        const float padZ = std::max(minWorld - (r.maxZ - r.minZ), 0.0f) * 0.5f;
        const float x0 = r.minX - padX, x1 = r.maxX + padX;
        const float z0 = r.minZ - padZ, z1 = r.maxZ + padZ;
        const XMFLOAT2 quad[4] = { map(x0, z0), map(x1, z0), map(x1, z1), map(x0, z1) };
        clipped(quad, 4, kRadarWall);
    }

    // A hairline along the arena's edge, over the terrain that runs up against
    // it. The edge is a wall a soldier can be pinned against, so it's worth
    // drawing as a line rather than left as wherever the dark fill stops.
    // Zoomed in it's mostly off the box, and what's left of it is the part
    // worth having: the reminder that this side of you is the end of the map.
    const float rule = std::max(1.0f * s, 1.0f);
    for (int i = 0; i < 4; ++i)
    {
        const XMFLOAT2& a = corners[i];
        const XMFLOAT2& b = corners[(i + 1) % 4];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 1e-4f)
            continue;
        const float px = -dy / len * rule * 0.5f, py = dx / len * rule * 0.5f;
        const XMFLOAT2 edge[4] = { { a.x - px, a.y - py },
                                   { b.x - px, b.y - py },
                                   { b.x + px, b.y + py },
                                   { a.x + px, a.y + py } };
        clipped(edge, 4, kDivider);
    }

    const float blipR = kBlipRadius * s;
    const float coreR = kBlipCore * s;

    for (size_t i = 0; i < radar.blipCount; ++i)
    {
        const Blip& blip = radar.blips[i];
        const XMFLOAT2 at = map(blip.x, blip.z);
        const float cx = at.x;
        const float cy = at.y;

        // Two colors, in the order the body wears them: the side underneath,
        // because which side a soldier is on is the only thing that decides
        // whether to shoot, and the class mark on top of it. Lifted toward
        // white on the way in for the same reason the roster's icons are —
        // armor is chosen to hold up at arena distance, and this is a ten-pixel
        // dot on a near-black panel.
        AppendClippedDisc(tris, cx, cy, blipR, fieldX, fieldY, fieldRight, fieldBottom,
                          Lift(blip.team, kSideLift));
        AppendClippedDisc(tris, cx, cy, coreR, fieldX, fieldY, fieldRight, fieldBottom,
                          Lift(blip.cls, kSideLift));

        if (!blip.you)
            continue;

        // You: the same two colors and a shape nobody else has. An arrowhead
        // off the front of the dot, in the class color that's already at its
        // middle, pointed where the soldier is pointed. The facing goes through
        // the same turn the position did — a heading drawn on the arena's axes
        // over a map drawn on the camera's would point somewhere the soldier
        // isn't looking, which is the whole fault this panel was turned to fix.
        // Normalized after that rather than before, since an aim direction
        // arriving as zero would otherwise put a degenerate triangle on screen.
        const float au = toU(blip.aimX, blip.aimZ), av = toV(blip.aimX, blip.aimZ);
        const float len = std::sqrt(au * au + av * av);
        if (len <= 1e-4f)
            continue;
        const float dx = au / len, dz = av / len;
        const float px = -dz, pz = dx;
        const float base = blipR * 0.55f;
        const float wing = blipR * 0.95f;
        const float reach = kBlipArrow * s;
        const XMFLOAT2 head[3] = { { cx + dx * reach, cy + dz * reach },
                                   { cx + dx * base + px * wing, cy + dz * base + pz * wing },
                                   { cx + dx * base - px * wing, cy + dz * base - pz * wing } };
        clipped(head, 3, Lift(blip.cls, kSideLift));
    }

    // The zoom keys, on the same caps the loadout's row uses, sat over the
    // panel's top edge and left-aligned with it — near enough the thing they
    // act on to be read as belonging to it, and out of the box rather than
    // over the ground, which at this size is the difference between a label
    // and an obstruction. OUT before IN, because that is the order the keys sit
    // in on the board and reading them the other way round would make the row
    // an instruction to be memorized rather than a picture of the two keys.
    DrawHints(renderer, tris, radar.hints, radar.hintCount, panelX,
              panelY - kHintGap * s - CapHeight(s), s, 1.0f);

    renderer.DrawScreenTriangles(tris.data(), static_cast<uint32_t>(tris.size()));
}
