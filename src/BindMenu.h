#pragma once

#include "Bindings.h"
#include "Input.h"
#include "Renderer.h"

#include <cstdint>
#include <vector>

// The settings screen for the key bindings: every action on its own row with
// the control it answers to, and a capture mode that takes the next thing the
// player touches.
//
// It edits the Bindings it's handed and does nothing else with them — it never
// loads or saves. Where a player's layout lives on disk is the game's business,
// and a screen that wrote the file itself would be a second thing deciding when
// that happens.
class BindMenu
{
public:
    // Returns true on the frame the player asks to leave.
    bool Update(const Input& input, Bindings& binds, uint32_t width, uint32_t height);

    void Render(Renderer& renderer, const Bindings& binds);

private:
    // The rows below the actions, in the order they're drawn. They're addressed
    // as rows past the last action so one cursor walks the whole screen.
    enum ExtraRow
    {
        RowReset,
        RowBack,
        ExtraRowCount
    };

    static constexpr size_t kRowCount = Bindings::kActionCount + ExtraRowCount;

    struct Rect
    {
        float x, y, w, h;
        bool Contains(float px, float py) const
        {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };

    static Rect RowRect(size_t index, float width, float height);

    int m_selected = 0;
    // The row whose control is being replaced, or -1 when nothing is being
    // captured. While it's set the screen reads nothing but the capture: a
    // click meant for a key binding must not also land on whatever row the
    // cursor is over.
    int m_capturing = -1;
    std::vector<Vertex> m_tris;
    std::vector<Vertex> m_lines;
};
