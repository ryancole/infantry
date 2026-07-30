#include "ClassSelect.h"

#include <algorithm>
#include <string>

using namespace DirectX;

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

    // Normalizers for the stat bars, chosen so the strongest class in each
    // stat fills its bar.
    constexpr float kMaxMoveSpeed = 11.0f;
    constexpr float kMaxFireRate = 1.0f / 0.12f;
    constexpr float kMaxProjectileSpeed = 80.0f;

    XMFLOAT4 Dim(const XMFLOAT4& c, float f)
    {
        return { c.x * f, c.y * f, c.z * f, c.w };
    }

    void AppendQuad(std::vector<Vertex>& out, float x, float y, float w, float h, float z,
                    const XMFLOAT4& color)
    {
        const Vertex v[4] = {
            { XMFLOAT3{ x, y, z }, color },
            { XMFLOAT3{ x + w, y, z }, color },
            { XMFLOAT3{ x + w, y + h, z }, color },
            { XMFLOAT3{ x, y + h, z }, color },
        };
        out.push_back(v[0]);
        out.push_back(v[1]);
        out.push_back(v[2]);
        out.push_back(v[0]);
        out.push_back(v[2]);
        out.push_back(v[3]);
    }

    void AppendOutline(std::vector<Vertex>& out, float x, float y, float w, float h, float z,
                       const XMFLOAT4& color)
    {
        const XMFLOAT3 p[4] = {
            { x, y, z }, { x + w, y, z }, { x + w, y + h, z }, { x, y + h, z }
        };
        for (int i = 0; i < 4; ++i)
        {
            out.push_back({ p[i], color });
            out.push_back({ p[(i + 1) % 4], color });
        }
    }

    void DrawCentered(Renderer& renderer, std::string_view text, float centerX, float y,
                      float size, const XMFLOAT4& color)
    {
        renderer.DrawScreenText(text, centerX - renderer.MeasureScreenText(text, size) * 0.5f, y,
                                size, color);
    }
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

        // Stat bars: label + background + class-colored fill.
        struct Bar
        {
            const char* label;
            float fraction;
        };
        const Bar bars[3] = {
            { "SPD", def.moveSpeed / kMaxMoveSpeed },
            { "ROF", (1.0f / def.fireInterval) / kMaxFireRate },
            { "RNG", def.projectileSpeed / kMaxProjectileSpeed },
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
