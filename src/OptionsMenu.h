#pragma once

#include "Input.h"
#include "Renderer.h"

#include <cstdint>
#include <optional>
#include <vector>

// The landing for everything the player can change about the game: one row per
// settings screen, and a way back.
//
// It exists so the main menu doesn't have to grow a row every time something
// becomes configurable. The front screen answers "am I playing"; this one
// answers "how", and a player looking for a setting has one door to try rather
// than reading the whole opening menu for the one entry that isn't about
// getting into a fight.
//
// Like the main menu it settles nothing itself — it hands back which screen was
// asked for and the game does the rest.
class OptionsMenu
{
public:
    enum class Choice
    {
        KeyBinds, // the rebinding screen
        Audio,    // sound settings
        Back,     // out to the main menu
    };

    // Returns the picked entry on the frame it's chosen, otherwise nullopt.
    // Escape and the pad's B report Back, the same as the row.
    std::optional<Choice> Update(const Input& input, uint32_t width, uint32_t height);

    // Draws the screen in screen-space; overrides the renderer's view-proj.
    void Render(Renderer& renderer);

private:
    struct Rect
    {
        float x, y, w, h;
        bool Contains(float px, float py) const
        {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };

    static Rect ItemRect(size_t index, float width, float height);

    int m_selected = 0;
    std::vector<Vertex> m_tris; // reused per frame
    std::vector<Vertex> m_lines;
};
