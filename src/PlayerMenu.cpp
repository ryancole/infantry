#include "PlayerMenu.h"

#include "PlayerName.h"
#include "ScreenDraw.h"

#include <algorithm>
#include <cmath>
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

    // Gathering the name off the keyboard by key rather than off a character
    // message, because that is the only kind of input this game has: everything
    // else it reads is a key that means an action, and the layer underneath
    // (DirectXTK's Keyboard) reports keys, not text. What that costs is exactly
    // the alphabet below — no accents, no other layouts, nothing shifted — and
    // what it buys is that a name is typed with the same object every other
    // screen is driven with.
    //
    // The join screen has one of these too, and deliberately not this one: an
    // address is spelled with dots and a name is spelled with spaces, and a
    // shared version would have to allow both everywhere. What may be in a name
    // is PlayerName's to say, so the keys are offered and it decides.
    void TypeInto(const Input& input, std::string& text)
    {
        const auto offer = [&](char c) {
            if (text.size() >= PlayerName::kMaxLength)
                return;
            // Every character offered below is already one a name may hold, so
            // the only rule left to keep is where a space may fall: not first,
            // and never two together. A space *last* is allowed while the
            // player is still typing — they're between two words, and a screen
            // that ate the gap they just pressed would be a screen fighting
            // them. It comes off on the way out (see Update), which is the
            // moment it stops being a gap and starts being a trailing space.
            if (c == ' ' && (text.empty() || text.back() == ' '))
                return;
            text.push_back(c);
        };

        for (int c = 'A'; c <= 'Z'; ++c)
            if (input.KeyPressed(c))
                offer(static_cast<char>(c));
        for (int c = '0'; c <= '9'; ++c)
            if (input.KeyPressed(c))
                offer(static_cast<char>(c));
        for (int i = 0; i <= 9; ++i)
            if (input.KeyPressed(VK_NUMPAD0 + i))
                offer(static_cast<char>('0' + i));
        if (input.KeyPressed(VK_SPACE))
            offer(' ');
        if (input.KeyPressed(VK_OEM_MINUS) || input.KeyPressed(VK_SUBTRACT))
            offer(input.Key(VK_SHIFT) ? '_' : '-');
        if (input.KeyPressed(VK_BACK) && !text.empty())
            text.pop_back();
    }
}

PlayerMenu::Rect PlayerMenu::RowRect(size_t index, float width, float height)
{
    // The audio screen's rows exactly: this is the other half of the same
    // settings shelf, and two screens behind adjacent doors should not be two
    // different sizes.
    const float rowW = width * 0.46f;
    const float rowH = height * 0.055f;
    const float step = height * 0.064f;
    const float gap = index >= RowBack ? height * 0.040f : 0.0f;
    return { (width - rowW) * 0.5f, height * 0.30f + index * step + gap, rowW, rowH };
}

bool PlayerMenu::Update(const Input& input, float dt, Settings& settings, uint32_t width,
                        uint32_t height)
{
    using PadTracker = DirectX::GamePad::ButtonStateTracker;

    m_blink += dt;

    // Every way out runs the name through the same rule the file and the wire
    // hold it to, which today means tidying away the gap somebody left on the
    // end of it. Done here rather than by the caller because it's the last
    // thing this screen is for: what leaves it is a name.
    const auto leave = [&] {
        settings.playerName = PlayerName::Clean(settings.playerName);
        return true;
    };

    if (input.KeyPressed(VK_ESCAPE) || input.padEvents.b == PadTracker::PRESSED)
        return leave();

    // Typing anywhere means the name row: reaching for a letter is a statement
    // of intent at least as clear as pointing at it.
    const size_t before = settings.playerName.size();
    TypeInto(input, settings.playerName);
    if (settings.playerName.size() != before)
        m_selected = RowName;

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    // Enter and the pad's A confirm; space does not, because on this screen
    // space is a character. Which is also why the arrows are the only way to
    // move the cursor — W and S are letters here.
    bool confirm = input.KeyPressed(VK_RETURN) || input.padEvents.a == PadTracker::PRESSED;

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
    const int rows = static_cast<int>(kRowCount);
    m_selected = ((m_selected + step) % rows + rows) % rows;

    // Confirming the name row does nothing at all, and shouldn't: the row is
    // already listening, and the change was made the moment the key was
    // pressed. There is nothing here to submit.
    return confirm && m_selected == RowBack ? leave() : false;
}

void PlayerMenu::Render(Renderer& renderer, const Settings& settings)
{
    const float w = static_cast<float>(renderer.Width());
    const float h = static_cast<float>(renderer.Height());
    renderer.SetViewProj(XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));

    m_tris.clear();
    m_lines.clear();

    DrawCentered(renderer, "PLAYER", w * 0.5f, h * 0.07f, h * 0.05f, kTitleColor);

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

        if (i == RowBack)
        {
            DrawCentered(renderer, "BACK", r.x + r.w * 0.5f, textY, textSize,
                         selected ? kTitleColor : Dim(kTitleColor, 0.6f));
            continue;
        }

        renderer.DrawScreenText("NAME", r.x + pad, textY, textSize,
                                selected ? kTitleColor : Dim(kTitleColor, 0.6f));
        // The same rule the other settings rows draw, carrying the eye from the
        // label on the left to the value on the right.
        AppendQuad(m_tris, r.x + pad, r.y + r.h * 0.76f, r.w - pad * 2.0f,
                   std::max(h * 0.0012f, 1.0f), kZFill, Dim(kHintColor, 0.35f));

        // The name is right-aligned like every other value on a settings row,
        // which puts the caret at the end of it — where the next letter is
        // going, rather than where the field happens to start.
        const bool empty = settings.playerName.empty();
        const std::string_view shown =
            empty ? std::string_view("UNNAMED") : std::string_view(settings.playerName);
        const float shownW = renderer.MeasureScreenText(shown, textSize);
        const float rightX = r.x + r.w - pad;
        renderer.DrawScreenText(shown, rightX - shownW, textY, textSize,
                                empty ? (selected ? kHintColor : Dim(kHintColor, 0.75f))
                                      : (selected ? kAccent : Dim(kAccent, 0.75f)));

        // The caret, blinking after whatever's typed: the one moving part that
        // says this row is listening to the keys.
        if (selected && std::fmod(m_blink, 1.0f) < 0.6f)
            AppendQuad(m_tris, rightX + textSize * 0.15f, textY, textSize * 0.12f, textSize,
                       kZText, kAccent);
    }

    // What the row means, under the list rather than in it — and the one thing
    // about it that would otherwise be a surprise: a name is stated at the door,
    // so changing it now is a change to the next fight rather than to this one.
    const Rect last = RowRect(kRowCount - 1, w, h);
    DrawCentered(renderer,
                 m_selected == RowName
                     ? "WHAT THE OTHER PLAYERS SEE - TAKES EFFECT THE NEXT TIME YOU JOIN"
                     : "LEAVE IT EMPTY AND THE SERVER WILL CALL YOU SOMETHING",
                 w * 0.5f, last.y + last.h + h * 0.06f, h * 0.022f, kHintColor);

    DrawCentered(renderer, "TYPE TO CHANGE - BACKSPACE TO ERASE - ESC TO GO BACK", w * 0.5f,
                 h * 0.92f, h * 0.022f, kHintColor);

    renderer.DrawTriangles(m_tris.data(), static_cast<uint32_t>(m_tris.size()),
                           XMMatrixIdentity());
    renderer.DrawLines(m_lines.data(), static_cast<uint32_t>(m_lines.size()),
                       XMMatrixIdentity());
}
