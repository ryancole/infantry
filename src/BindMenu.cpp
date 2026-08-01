#include "BindMenu.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <string>

using namespace DirectX;
using namespace ScreenDraw;

namespace
{
    // Same layers and palette as the other full-screen menus.
    constexpr float kZFill = 0.8f;
    constexpr float kZText = 0.6f;

    constexpr XMFLOAT4 kTitleColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    constexpr XMFLOAT4 kRowBg = { 0.06f, 0.08f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kRowBgSelected = { 0.10f, 0.14f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kAccent = { 0.35f, 0.75f, 0.95f, 1.0f };
    // What a row waiting for a key says. Its own color because it isn't a
    // binding — it's the screen asking a question, and reading it as the name
    // of some key called PRESS A KEY is a mistake worth one constant to avoid.
    constexpr XMFLOAT4 kCaptureColor = { 0.95f, 0.75f, 0.30f, 1.0f };

    constexpr const char* kExtraLabels[] = { "RESET TO DEFAULTS", "BACK" };

    void DrawRight(Renderer& renderer, std::string_view text, float rightX, float y, float size,
                   const XMFLOAT4& color)
    {
        renderer.DrawScreenText(text, rightX - renderer.MeasureScreenText(text, size), y, size,
                                color);
    }
}

BindMenu::Rect BindMenu::RowRect(size_t index, float width, float height)
{
    const float rowW = width * 0.46f;
    const float rowH = height * 0.042f;
    const float step = height * 0.047f;
    // The two command rows sit under a gap: they act on the list rather than
    // being part of it, and running them straight on makes RESET TO DEFAULTS
    // look like one more thing that can be bound.
    const float gap = index >= Bindings::kActionCount ? height * 0.030f : 0.0f;
    return { (width - rowW) * 0.5f, height * 0.16f + index * step + gap, rowW, rowH };
}

bool BindMenu::Update(const Input& input, Bindings& binds, uint32_t width, uint32_t height)
{
    using PadTracker = DirectX::GamePad::ButtonStateTracker;

    // Capture owns the frame. Nothing else may read the input while it's
    // running: the button the player is binding is also a button this screen
    // navigates with, and only one of those can be true at a time.
    if (m_capturing >= 0)
    {
        if (input.KeyPressed(VK_ESCAPE))
        {
            m_capturing = -1;
            return false;
        }
        if (const auto bind = Bindings::CaptureAny(input))
        {
            binds.Set(static_cast<Bindings::Action>(m_capturing), *bind);
            m_capturing = -1;
        }
        return false;
    }

    if (input.KeyPressed(VK_ESCAPE) || input.padEvents.b == PadTracker::PRESSED)
        return true;

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    bool confirm = input.KeyPressed(VK_RETURN) || input.KeyPressed(VK_SPACE) ||
                   input.padEvents.a == PadTracker::PRESSED;

    for (size_t i = 0; i < kRowCount; ++i)
    {
        if (RowRect(i, w, h).Contains(input.mouseX, input.mouseY))
        {
            m_selected = static_cast<int>(i);
            if (input.MousePressed(0))
                confirm = true;
        }
    }

    int step = 0;
    if (input.KeyPressed(VK_DOWN) || input.padEvents.dpadDown == PadTracker::PRESSED ||
        input.padEvents.leftStickDown == PadTracker::PRESSED)
        ++step;
    if (input.KeyPressed(VK_UP) || input.padEvents.dpadUp == PadTracker::PRESSED ||
        input.padEvents.leftStickUp == PadTracker::PRESSED)
        --step;
    // W and S aren't in here the way they are on the main menu: they're on the
    // list as bindings, and a screen where the movement keys scroll the list of
    // movement keys is a screen that argues with itself.
    const int rows = static_cast<int>(kRowCount);
    m_selected = ((m_selected + step) % rows + rows) % rows;

    if (!confirm)
        return false;

    if (m_selected < static_cast<int>(Bindings::kActionCount))
    {
        m_capturing = m_selected;
        return false;
    }
    switch (m_selected - static_cast<int>(Bindings::kActionCount))
    {
    case RowReset: binds.ResetToDefaults(); return false;
    case RowBack:  return true;
    }
    return false;
}

void BindMenu::Render(Renderer& renderer, const Bindings& binds)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "KEY BINDS", w * 0.5f, h * 0.07f, h * 0.05f, kTitleColor);

    for (size_t i = 0; i < kRowCount; ++i)
    {
        const Rect r = RowRect(i, w, h);
        const bool selected = static_cast<int>(i) == m_selected;
        const bool capturing = static_cast<int>(i) == m_capturing;
        const float pad = r.h * 0.45f;
        const float textSize = r.h * 0.46f;
        const float textY = r.y + (r.h - textSize) * 0.5f;

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZFill, selected ? kRowBgSelected : kRowBg);
        if (selected)
            AppendOutline(m_lines, r.x, r.y, r.w, r.h, kZText, capturing ? kCaptureColor : kAccent);

        if (i >= Bindings::kActionCount)
        {
            DrawCentered(renderer, kExtraLabels[i - Bindings::kActionCount], r.x + r.w * 0.5f,
                         textY, textSize, selected ? kTitleColor : Dim(kTitleColor, 0.6f));
            continue;
        }

        const Bindings::Action action = static_cast<Bindings::Action>(i);
        renderer.DrawScreenText(Bindings::Name(action), r.x + pad, textY, textSize,
                                selected ? kTitleColor : Dim(kTitleColor, 0.6f));
        // The dots are what carry the eye from a short name on the left to a
        // short label on the right across a row that's mostly empty.
        AppendQuad(m_tris, r.x + pad, r.y + r.h * 0.72f, r.w - pad * 2.0f,
                   std::max(h * 0.0012f, 1.0f), kZFill, Dim(kHintColor, 0.35f));

        if (capturing)
            DrawRight(renderer, "PRESS A KEY", r.x + r.w - pad, textY, textSize, kCaptureColor);
        else
            DrawRight(renderer, binds.Label(action), r.x + r.w - pad, textY, textSize,
                      selected ? kAccent : Dim(kAccent, 0.75f));
    }

    DrawCentered(renderer,
                 m_capturing >= 0 ? "PRESS ANY KEY OR MOUSE BUTTON - ESC TO CANCEL"
                                  : "ENTER OR CLICK TO REBIND - ESC TO GO BACK",
                 w * 0.5f, h * 0.92f, h * 0.022f, m_capturing >= 0 ? kCaptureColor : kHintColor);

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
