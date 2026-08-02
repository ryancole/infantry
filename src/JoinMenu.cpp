#include "JoinMenu.h"

#include "ScreenDraw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DirectX;
using namespace ScreenDraw;

namespace
{
    // Screen-space depth layers (ortho z, smaller is closer), matching the
    // other menus: fills at the back, lines and text on top.
    constexpr float kZFill = 0.8f;
    constexpr float kZText = 0.6f;

    constexpr XMFLOAT4 kTitleColor = { 0.85f, 0.90f, 0.95f, 1.0f };
    constexpr XMFLOAT4 kHintColor = { 0.45f, 0.52f, 0.62f, 1.0f };
    constexpr XMFLOAT4 kItemBg = { 0.06f, 0.08f, 0.12f, 1.0f };
    constexpr XMFLOAT4 kItemBgSelected = { 0.10f, 0.14f, 0.20f, 1.0f };
    constexpr XMFLOAT4 kAccent = { 0.35f, 0.75f, 0.95f, 1.0f };

    constexpr size_t kMaxAddress = 24; // fits the row; a hostname, not an essay

    // The characters an address can be spelled with, gathered off the
    // keyboard by key. The font is uppercase-only and so are hostnames as
    // far as this screen cares; both halves of the numeric row and the
    // numpad spell the same digits.
    void TypeInto(const Input& input, std::string& text)
    {
        for (int c = 'A'; c <= 'Z'; ++c)
            if (input.KeyPressed(c) && text.size() < kMaxAddress)
                text.push_back(static_cast<char>(c));
        for (int c = '0'; c <= '9'; ++c)
            if (input.KeyPressed(c) && text.size() < kMaxAddress)
                text.push_back(static_cast<char>(c));
        for (int i = 0; i <= 9; ++i)
            if (input.KeyPressed(VK_NUMPAD0 + i) && text.size() < kMaxAddress)
                text.push_back(static_cast<char>('0' + i));
        if ((input.KeyPressed(VK_OEM_PERIOD) || input.KeyPressed(VK_DECIMAL)) &&
            text.size() < kMaxAddress)
            text.push_back('.');
        if ((input.KeyPressed(VK_OEM_MINUS) || input.KeyPressed(VK_SUBTRACT)) &&
            text.size() < kMaxAddress)
            text.push_back('-');
        if (input.KeyPressed(VK_BACK) && !text.empty())
            text.pop_back();
    }
}

JoinMenu::Rect JoinMenu::RowRect(size_t index, float width, float height)
{
    const float itemW = width * 0.44f;
    const float itemH = height * 0.068f;
    const float gap = height * 0.018f;
    return { (width - itemW) * 0.5f, height * 0.34f + index * (itemH + gap), itemW, itemH };
}

std::optional<std::string> JoinMenu::Update(const Input& input, float dt,
                                            const std::vector<Discovery::ServerInfo>& servers,
                                            uint32_t width, uint32_t height)
{
    m_blink += dt;

    const size_t shown = std::min(servers.size(), kMaxRows);
    const int addressRow = static_cast<int>(shown); // always the last row
    const int count = addressRow + 1;
    m_selected = std::clamp(m_selected, 0, count - 1);

    // Typing anywhere means the typed row: reaching for the address is a
    // statement of intent at least as clear as pointing at it.
    const size_t before = m_address.size();
    TypeInto(input, m_address);
    if (m_address.size() != before)
        m_selected = addressRow;

    // Mouse first, same reasoning as the main menu: the pointer is the more
    // recent statement of intent whenever it's actually over something.
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (int i = 0; i < count; ++i)
    {
        if (RowRect(static_cast<size_t>(i), w, h).Contains(input.mouseX, input.mouseY))
        {
            m_selected = i;
            if (input.MousePressed(0))
            {
                if (i < addressRow)
                    return servers[static_cast<size_t>(i)].host;
                if (!m_address.empty())
                    return m_address;
            }
        }
    }

    using PadTracker = DirectX::GamePad::ButtonStateTracker;
    int step = 0;
    if (input.KeyPressed(VK_DOWN) || input.padEvents.dpadDown == PadTracker::PRESSED ||
        input.padEvents.leftStickDown == PadTracker::PRESSED)
        ++step;
    if (input.KeyPressed(VK_UP) || input.padEvents.dpadUp == PadTracker::PRESSED ||
        input.padEvents.leftStickUp == PadTracker::PRESSED)
        --step;
    m_selected = ((m_selected + step) % count + count) % count;

    if (input.KeyPressed(VK_RETURN) || input.padEvents.a == PadTracker::PRESSED)
    {
        if (m_selected < addressRow)
            return servers[static_cast<size_t>(m_selected)].host;
        if (!m_address.empty())
            return m_address;
    }

    return std::nullopt;
}

void JoinMenu::Render(Renderer& renderer, const std::vector<Discovery::ServerInfo>& servers)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "JOIN A SERVER", w * 0.5f, h * 0.14f, h * 0.055f, kTitleColor);
    // The scan never stops while the screen is up, and saying so is what
    // makes an empty list read as "still looking" instead of "nobody's out
    // there, go home".
    DrawCentered(renderer,
                 servers.empty() ? "SCANNING THE LAN - NOTHING ANSWERING YET"
                                 : "SCANNING THE LAN",
                 w * 0.5f, h * 0.235f, h * 0.022f, kHintColor);

    const Rect first = RowRect(0, w, h);
    AppendQuad(m_tris, first.x, h * 0.30f, first.w, std::max(h * 0.002f, 1.0f), kZFill,
               Dim(kAccent, 0.35f));

    const size_t shown = std::min(servers.size(), kMaxRows);
    const int addressRow = static_cast<int>(shown);

    for (size_t i = 0; i < shown; ++i)
    {
        const Rect r = RowRect(i, w, h);
        const bool selected = static_cast<int>(i) == m_selected;
        const Discovery::ServerInfo& info = servers[i];

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZFill, selected ? kItemBgSelected : kItemBg);
        AppendOutline(m_lines, r.x, r.y, r.w, r.h, kZText,
                      selected ? kAccent : Dim(kAccent, 0.35f));

        const float pad = r.w * 0.03f;
        const float nameSize = r.h * 0.34f;
        const XMFLOAT4 nameColor = selected ? kTitleColor : Dim(kTitleColor, 0.6f);
        renderer.DrawScreenText(info.name, r.x + pad, r.y + r.h * 0.18f, nameSize, nameColor);

        char detail[64];
        std::snprintf(detail, sizeof detail, "%s   %d/%d", info.host.c_str(), info.humans,
                      info.capacity);
        const float detailSize = r.h * 0.24f;
        renderer.DrawScreenText(detail, r.x + pad, r.y + r.h * 0.60f, detailSize,
                                selected ? kHintColor : Dim(kHintColor, 0.7f));
    }

    // The typed row: for the server the shout can't reach.
    {
        const Rect r = RowRect(static_cast<size_t>(addressRow), w, h);
        const bool selected = m_selected == addressRow;

        AppendQuad(m_tris, r.x, r.y, r.w, r.h, kZFill, selected ? kItemBgSelected : kItemBg);
        AppendOutline(m_lines, r.x, r.y, r.w, r.h, kZText,
                      selected ? kAccent : Dim(kAccent, 0.35f));

        const float pad = r.w * 0.03f;
        renderer.DrawScreenText("CONNECT TO ADDRESS", r.x + pad, r.y + r.h * 0.16f,
                                r.h * 0.24f, selected ? kHintColor : Dim(kHintColor, 0.7f));

        const float textSize = r.h * 0.34f;
        const XMFLOAT4 textColor =
            m_address.empty() ? Dim(kHintColor, 0.8f) : (selected ? kTitleColor : Dim(kTitleColor, 0.6f));
        const std::string_view shownText =
            m_address.empty() ? std::string_view("TYPE AN IP OR NAME") : std::string_view(m_address);
        renderer.DrawScreenText(shownText, r.x + pad, r.y + r.h * 0.52f, textSize, textColor);

        // The caret, blinking on the selected row after whatever's typed —
        // the one moving part that says "this row is listening to the keys".
        if (selected && std::fmod(m_blink, 1.0f) < 0.6f)
        {
            const float caretX =
                r.x + pad +
                (m_address.empty() ? 0.0f
                                   : renderer.MeasureScreenText(m_address, textSize) +
                                         textSize * 0.15f);
            AppendQuad(m_tris, caretX, r.y + r.h * 0.52f, textSize * 0.12f, textSize, kZText,
                       kAccent);
        }
    }

    DrawCentered(renderer, "PICK A SERVER OR TYPE AN ADDRESS - ENTER TO CONNECT - ESC TO BACK OUT",
                 w * 0.5f, h * 0.88f, h * 0.022f, kHintColor);

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
