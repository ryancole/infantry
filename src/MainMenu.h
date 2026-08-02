#pragma once

#include "Input.h"
#include "Renderer.h"

#include <cstdint>
#include <optional>
#include <vector>

// The screen the game opens on. It settles nothing about the soldier — that's
// still the class select's job, one step further in — it settles whether the
// player is going in at all, which is the question a game that drops you
// straight into a class card never gets to ask.
//
// It's deliberately the thinnest thing that earns the name: a title, and the
// two answers there are today. Everything a menu system eventually grows
// (settings, a server list, a profile) is another row in the table this drives
// itself off, and the phase it hands back to the game is the only thing that
// has to learn about it.
class MainMenu
{
public:
    enum class Choice
    {
        Deploy,   // on to the class select, and a match of our own
        Join,     // the server browser: somebody else's match
        KeyBinds, // the rebinding screen
        Quit,     // close the game
    };

    // Returns the picked entry on the frame it's chosen, otherwise nullopt.
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

    // One cursor for both hands: the keys and the stick step it, and the mouse
    // moves it by hovering rather than lighting up a second highlight of its
    // own. There is only ever one entry the game considers chosen, so there is
    // only ever one entry drawn that way.
    int m_selected = 0;
    std::vector<Vertex> m_tris;  // reused per frame
    std::vector<Vertex> m_lines;
};
