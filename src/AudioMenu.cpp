#include "AudioMenu.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <array>
#include <string_view>

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
    // OFF is drawn cooler than ON rather than just dimmer, so the state of a
    // row is readable without reading the word.
    constexpr XMFLOAT4 kOffColor = { 0.40f, 0.46f, 0.55f, 1.0f };

    // One row per toggle, in the order AudioMenu::Row lists them, and the only
    // place any of these three facts is written down: the words the player
    // reads, the sentence explaining what it costs them, and which field it
    // sets. A member pointer rather than a switch so adding a setting is a line
    // here and a line in the enum, and neither one can be forgotten quietly.
    struct Toggle
    {
        const char* name;
        const char* blurb;
        bool Settings::* field;
    };

    constexpr std::array<Toggle, 1> kToggles = { {
        { "PLAY IN BACKGROUND", "KEEP THE SOUND ON WHILE ANOTHER WINDOW HAS FOCUS",
          &Settings::backgroundAudio },
    } };

    void DrawRight(Renderer& renderer, std::string_view text, float rightX, float y, float size,
                   const XMFLOAT4& color)
    {
        renderer.DrawScreenText(text, rightX - renderer.MeasureScreenText(text, size), y, size,
                                color);
    }
}

AudioMenu::Rect AudioMenu::RowRect(size_t index, float width, float height)
{
    const float rowW = width * 0.46f;
    const float rowH = height * 0.055f;
    const float step = height * 0.064f;
    // BACK sits under a gap: it acts on the screen rather than being one more
    // thing that can be switched on.
    const float gap = index >= kToggleCount ? height * 0.040f : 0.0f;
    return { (width - rowW) * 0.5f, height * 0.30f + index * step + gap, rowW, rowH };
}

bool AudioMenu::Update(const Input& input, Settings& settings, uint32_t width, uint32_t height)
{
    static_assert(kToggles.size() == kToggleCount, "the toggle table and Row must agree");

    using PadTracker = DirectX::GamePad::ButtonStateTracker;

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
    if (input.KeyPressed(VK_DOWN) || input.KeyPressed('S') ||
        input.padEvents.dpadDown == PadTracker::PRESSED ||
        input.padEvents.leftStickDown == PadTracker::PRESSED)
        ++step;
    if (input.KeyPressed(VK_UP) || input.KeyPressed('W') ||
        input.padEvents.dpadUp == PadTracker::PRESSED ||
        input.padEvents.leftStickUp == PadTracker::PRESSED)
        --step;
    const int rows = static_cast<int>(kRowCount);
    m_selected = ((m_selected + step) % rows + rows) % rows;

    if (!confirm)
        return false;

    if (m_selected < static_cast<int>(kToggleCount))
    {
        // A toggle is its own confirmation: the row says what it's set to, so
        // flipping it is the whole interaction and the change is visible on
        // the frame it happens.
        bool& field = settings.*kToggles[static_cast<size_t>(m_selected)].field;
        field = !field;
        return false;
    }
    return true; // RowBack
}

void AudioMenu::Render(Renderer& renderer, const Settings& settings)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "AUDIO", w * 0.5f, h * 0.07f, h * 0.05f, kTitleColor);

    for (size_t i = 0; i < kRowCount; ++i)
    {
        const Rect r = RowRect(i, w, h);
        const bool selected = static_cast<int>(i) == m_selected;
        const float pad = r.h * 0.45f;
        const float textSize = r.h * 0.40f;
        const float textY = r.y + (r.h - textSize) * 0.5f;

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZFill, selected ? kRowBgSelected : kRowBg);
        if (selected)
            AppendOutline(m_lines, r.x, r.y, r.w, r.h, kZText, kAccent);

        if (i >= kToggleCount)
        {
            DrawCentered(renderer, "BACK", r.x + r.w * 0.5f, textY, textSize,
                         selected ? kTitleColor : Dim(kTitleColor, 0.6f));
            continue;
        }

        const Toggle& toggle = kToggles[i];
        const bool on = settings.*toggle.field;

        renderer.DrawScreenText(toggle.name, r.x + pad, textY, textSize,
                                selected ? kTitleColor : Dim(kTitleColor, 0.6f));
        // The same rule the key binds draw, carrying the eye from the name on
        // the left to the state on the right.
        AppendQuad(m_tris, r.x + pad, r.y + r.h * 0.76f, r.w - pad * 2.0f,
                   std::max(h * 0.0012f, 1.0f), kZFill, Dim(kHintColor, 0.35f));

        DrawRight(renderer, on ? "ON" : "OFF", r.x + r.w - pad, textY, textSize,
                  on ? (selected ? kAccent : Dim(kAccent, 0.75f))
                     : (selected ? kOffColor : Dim(kOffColor, 0.75f)));
    }

    // What the selected row actually does, under the list rather than in it: a
    // sentence per row would turn four settings into a page of prose, and the
    // one being considered is the only one whose meaning is in question.
    if (m_selected < static_cast<int>(kToggleCount))
    {
        const Rect last = RowRect(kRowCount - 1, w, h);
        DrawCentered(renderer, kToggles[static_cast<size_t>(m_selected)].blurb, w * 0.5f,
                     last.y + last.h + h * 0.06f, h * 0.022f, kHintColor);
    }

    DrawCentered(renderer, "ENTER OR CLICK TO SWITCH - ESC TO GO BACK", w * 0.5f, h * 0.92f,
                 h * 0.022f, kHintColor);

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
