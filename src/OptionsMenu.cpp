#include "OptionsMenu.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <array>

using namespace DirectX;
using namespace ScreenDraw;

namespace
{
    // Same layers and palette as the other full-screen menus.
    constexpr float kZFill = 0.8f;
    constexpr float kZText = 0.6f;

    constexpr XMFLOAT4 kTitleColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    constexpr XMFLOAT4 kItemBg = { 0.06f, 0.08f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kItemBgSelected = { 0.10f, 0.14f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kAccent = { 0.35f, 0.75f, 0.95f, 1.0f };

    struct Item
    {
        const char* label;
        const char* blurb;
        OptionsMenu::Choice choice;
    };

    constexpr std::array<Item, 3> kItems = { {
        { "KEY BINDS", "PUT THE CONTROLS WHERE YOU WANT THEM", OptionsMenu::Choice::KeyBinds },
        { "AUDIO", "DECIDE WHEN THE GAME MAKES NOISE", OptionsMenu::Choice::Audio },
        { "BACK", "RETURN TO THE MAIN MENU", OptionsMenu::Choice::Back },
    } };
}

OptionsMenu::Rect OptionsMenu::ItemRect(size_t index, float width, float height)
{
    // The main menu's rows, shifted up to sit under a title of their own. Same
    // shape on purpose: this is the screen behind one of those rows, and a
    // different box size would read as a different kind of place.
    const float itemW = width * 0.28f;
    const float itemH = height * 0.082f;
    const float gap = height * 0.026f;
    return { (width - itemW) * 0.5f, height * 0.30f + index * (itemH + gap), itemW, itemH };
}

std::optional<OptionsMenu::Choice> OptionsMenu::Update(const Input& input, uint32_t width,
                                                       uint32_t height)
{
    using PadTracker = DirectX::GamePad::ButtonStateTracker;

    if (input.KeyPressed(VK_ESCAPE) || input.padEvents.b == PadTracker::PRESSED)
        return Choice::Back;

    const int count = static_cast<int>(kItems.size());

    // Mouse first, for the same reason the main menu does it: a cursor resting
    // on an entry is the more recent statement of intent.
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (size_t i = 0; i < kItems.size(); ++i)
    {
        if (ItemRect(i, w, h).Contains(input.mouseX, input.mouseY))
        {
            m_selected = static_cast<int>(i);
            if (input.MousePressed(0))
                return kItems[i].choice;
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
    m_selected = ((m_selected + step) % count + count) % count;

    if (input.KeyPressed(VK_RETURN) || input.KeyPressed(VK_SPACE) ||
        input.padEvents.a == PadTracker::PRESSED)
        return kItems[static_cast<size_t>(m_selected)].choice;

    return std::nullopt;
}

void OptionsMenu::Render(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "OPTIONS", w * 0.5f, h * 0.07f, h * 0.05f, kTitleColor);

    // The same rule the main menu draws under its title, so the column reads as
    // one block rather than a heading with a list under it.
    const Rect first = ItemRect(0, w, h);
    AppendQuad(m_tris, first.x, h * 0.265f, first.w, std::max(h * 0.002f, 1.0f), kZFill,
               Dim(kAccent, 0.35f));

    for (size_t i = 0; i < kItems.size(); ++i)
    {
        const Rect r = ItemRect(i, w, h);
        const bool selected = static_cast<int>(i) == m_selected;

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZFill, selected ? kItemBgSelected : kItemBg);
        AppendOutline(m_lines, r.x, r.y, r.w, r.h, kZText,
                      selected ? kAccent : Dim(kAccent, 0.35f));

        DrawCentered(renderer, kItems[i].label, r.x + r.w * 0.5f, r.y + r.h * 0.26f, r.h * 0.42f,
                     selected ? kTitleColor : Dim(kTitleColor, 0.6f));

        if (selected)
        {
            DrawCentered(renderer, kItems[i].blurb, r.x + r.w * 0.5f, r.y + r.h * 0.72f,
                         r.h * 0.18f, kHintColor);

            const float size = r.h * 0.30f;
            const float x = r.x - size * 1.6f;
            const float y = r.y + (r.h - size) * 0.5f;
            m_tris.push_back({ XMFLOAT3{ x, y, kZText }, kAccent });
            m_tris.push_back({ XMFLOAT3{ x + size * 0.8f, y + size * 0.5f, kZText }, kAccent });
            m_tris.push_back({ XMFLOAT3{ x, y + size, kZText }, kAccent });
        }
    }

    DrawCentered(renderer, "ARROWS OR MOUSE TO CHOOSE - ENTER TO CONFIRM - ESC TO GO BACK",
                 w * 0.5f, h * 0.92f, h * 0.022f, kHintColor);

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
