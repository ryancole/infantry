#pragma once

#include "Input.h"
#include "Renderer.h"
#include "Settings.h"

#include <cstdint>
#include <vector>

// The sound settings, laid out the way the key binds are: one row per thing
// that can be changed, what it's currently set to on the right, and a row out
// at the bottom.
//
// Like BindMenu it edits what it's handed and does nothing else with it — it
// never loads, never saves, and never touches the audio engine. When a toggle
// takes effect is the game's business (see Game::Update), which is what keeps
// the screen from being a second opinion about what the ear is doing.
class AudioMenu
{
public:
    // Returns true on the frame the player asks to leave.
    bool Update(const Input& input, Settings& settings, uint32_t width, uint32_t height);

    void Render(Renderer& renderer, const Settings& settings);

private:
    // The rows in the order they're drawn. The toggles come first and BACK
    // sits past them, so one cursor walks the whole screen — the same shape
    // BindMenu uses for its command rows.
    enum Row
    {
        RowBackgroundAudio,
        RowBack,
        RowCount
    };

    static constexpr size_t kToggleCount = RowBack;
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

    int m_selected = 0;
    std::vector<Vertex> m_tris;
    std::vector<Vertex> m_lines;
};
