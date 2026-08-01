#include "MainMenu.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <array>

using namespace DirectX;
using namespace ScreenDraw;

namespace
{
    // Screen-space depth layers (ortho z, smaller is closer), matching the
    // class select: fills at the back, lines and text on top.
    constexpr float kZFill = 0.8f;
    constexpr float kZText = 0.6f;

    constexpr XMFLOAT4 kTitleColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    constexpr XMFLOAT4 kItemBg = { 0.06f, 0.08f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kItemBgSelected = { 0.10f, 0.14f, 0.20f, 1.0f };
    // The menu's own accent. It isn't any class's color on purpose: nothing has
    // been chosen yet at this point, and borrowing the marine's green here would
    // quietly say otherwise.
    constexpr XMFLOAT4 kAccent = { 0.35f, 0.75f, 0.95f, 1.0f };

    struct Item
    {
        const char* label;
        const char* blurb;
        MainMenu::Choice choice;
    };

    constexpr std::array<Item, 3> kItems = { {
        { "DEPLOY", "PICK A CLASS AND TAKE THE FIELD", MainMenu::Choice::Deploy },
        { "KEY BINDS", "PUT THE CONTROLS WHERE YOU WANT THEM", MainMenu::Choice::KeyBinds },
        { "QUIT", "LEAVE THE ARENA", MainMenu::Choice::Quit },
    } };
}

MainMenu::Rect MainMenu::ItemRect(size_t index, float width, float height)
{
    const float itemW = width * 0.28f;
    const float itemH = height * 0.09f;
    const float gap = height * 0.035f;
    return { (width - itemW) * 0.5f, height * 0.46f + index * (itemH + gap), itemW, itemH };
}

std::optional<MainMenu::Choice> MainMenu::Update(const Input& input, uint32_t width,
                                                 uint32_t height)
{
    const int count = static_cast<int>(kItems.size());

    // Mouse first, so a cursor resting on an entry wins over wherever the keys
    // last left the cursor — the pointer is the more recent statement of intent
    // whenever it's actually over something.
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

    using PadTracker = DirectX::GamePad::ButtonStateTracker;
    int step = 0;
    if (input.KeyPressed(VK_DOWN) || input.KeyPressed('S') ||
        input.padEvents.dpadDown == PadTracker::PRESSED ||
        input.padEvents.leftStickDown == PadTracker::PRESSED)
        ++step;
    if (input.KeyPressed(VK_UP) || input.KeyPressed('W') ||
        input.padEvents.dpadUp == PadTracker::PRESSED ||
        input.padEvents.leftStickUp == PadTracker::PRESSED)
        --step;
    // Wraps, because a two-entry list with a dead end at each end is a list
    // that punishes holding the key a beat too long for no reason.
    m_selected = ((m_selected + step) % count + count) % count;

    if (input.KeyPressed(VK_RETURN) || input.KeyPressed(VK_SPACE) ||
        input.padEvents.a == PadTracker::PRESSED)
        return kItems[static_cast<size_t>(m_selected)].choice;

    return std::nullopt;
}

void MainMenu::Render(Renderer& renderer)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "INFANTRY", w * 0.5f, h * 0.20f, h * 0.11f, kTitleColor);
    DrawCentered(renderer, "PROTOTYPE", w * 0.5f, h * 0.34f, h * 0.028f, kHintColor);

    // A rule between the name and the choices, the width of the widest entry so
    // the block reads as one column rather than a title with a list under it.
    const Rect first = ItemRect(0, w, h);
    AppendQuad(m_tris, first.x, h * 0.40f, first.w, std::max(h * 0.002f, 1.0f), kZFill,
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

        // The blurb only shows on the entry under the cursor. All of them at
        // once turns a two-line menu into a paragraph, and the one being
        // considered is the only one whose consequences are being weighed.
        if (selected)
            DrawCentered(renderer, kItems[i].blurb, r.x + r.w * 0.5f, r.y + r.h * 0.72f,
                         r.h * 0.18f, kHintColor);

        // A caret outside the box on the selected row: the fill and outline
        // shift is easy to miss on a dark screen, and this is legible at a
        // glance from across a desk.
        if (selected)
        {
            const float size = r.h * 0.30f;
            const float x = r.x - size * 1.6f;
            const float y = r.y + (r.h - size) * 0.5f;
            m_tris.push_back({ XMFLOAT3{ x, y, kZText }, kAccent });
            m_tris.push_back({ XMFLOAT3{ x + size * 0.8f, y + size * 0.5f, kZText }, kAccent });
            m_tris.push_back({ XMFLOAT3{ x, y + size, kZText }, kAccent });
        }
    }

    DrawCentered(renderer, "ARROWS OR MOUSE TO CHOOSE - ENTER TO CONFIRM", w * 0.5f, h * 0.86f,
                 h * 0.022f, kHintColor);

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
