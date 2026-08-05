#pragma once

#include "Input.h"
#include "Renderer.h"
#include "Settings.h"

#include <cstdint>
#include <vector>

// Who the player is, which is one row long: the name they fight under, and a
// way back. Laid out like the audio settings — a label on the left, what it's
// currently set to on the right — because it is the same kind of screen and a
// player who has seen one should recognise the other.
//
// It differs from every other menu in the game in one way that matters: the
// keyboard is being typed on rather than steered with. So there is no W/S
// alternative to the arrow keys here (W and S are letters somebody's name might
// need), space types a space instead of confirming, and reaching for a letter
// selects the row that's listening — the same rule the join screen's typed
// address follows, for the same reason.
//
// Like BindMenu and AudioMenu it edits what it's handed and does nothing else:
// it never loads, never saves, and never tells anybody the name has changed.
// When that's written to disk is the game's business (Game::Update), and when
// it reaches a server is the wire's — a name goes up at the door, so a change
// made here lands the next time the player joins one.
class PlayerMenu
{
public:
    // Returns true on the frame the player asks to leave. `dt` drives the
    // caret and nothing else.
    bool Update(const Input& input, float dt, Settings& settings, uint32_t width,
                uint32_t height);

    void Render(Renderer& renderer, const Settings& settings);

private:
    // The rows in the order they're drawn, the same shape AudioMenu uses: the
    // thing being set first, and BACK past it, so one cursor walks the screen.
    enum Row
    {
        RowName,
        RowBack,
        RowCount
    };

    static constexpr size_t kRowCount = RowCount;

    struct Rect
    {
        float x, y, w, h;
        bool Contains(float px, float py) const
        {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };

    static Rect RowRect(size_t index, float width, float height);

    int m_selected = RowName;
    float m_blink = 0.0f;
    std::vector<Vertex> m_tris;
    std::vector<Vertex> m_lines;
};
