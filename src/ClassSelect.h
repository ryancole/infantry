#pragma once

#include "Input.h"
#include "PlayerClass.h"
#include "Renderer.h"

#include <cstdint>
#include <optional>
#include <vector>

// The forced pre-spawn class selection screen: one card per class with name,
// role blurb, and stat bars. The player picks by clicking a card or pressing
// its number key; until then the game doesn't spawn them into the arena.
class ClassSelect
{
public:
    // Returns the chosen class on the frame it's picked, otherwise nullopt.
    std::optional<ClassId> Update(const Input& input, uint32_t width, uint32_t height);

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

    static Rect CardRect(size_t index, float width, float height);

    int m_hover = -1;
    std::vector<Vertex> m_tris;  // reused per frame
    std::vector<Vertex> m_lines;
};
