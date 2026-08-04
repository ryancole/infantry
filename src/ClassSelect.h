#pragma once

#include "Bindings.h"
#include "Input.h"
#include "PlayerClass.h"
#include "Renderer.h"

#include <cstdint>
#include <optional>
#include <vector>

// The class selection screen: one card per class with name, role blurb, and
// stat bars. The player picks by clicking a card or pressing its number key.
//
// It is shown twice over a match's life, and it is deliberately the same
// screen both times. Once before spawning, where it's a gate — until a class
// is picked there is nobody to put on the field. And once from inside the
// match, over the arena, when the player is standing on their own spawn or
// waiting to come back: a class is what you're committed to for a life, and
// those are the two places a life can be started over. The cards can't differ
// between the two, because the choice doesn't.
class ClassSelect
{
public:
    // Which of the two it is. It changes nothing about the cards and everything
    // about their edges: what the screen is called, how to back out of it, and
    // whether there is a match behind it that needs dimming to read them
    // against.
    enum class Mode
    {
        Spawn,  // before the player has taken the field; nothing behind it
        Change, // in the match, over the top of it
    };

    // Returns the chosen class on the frame it's picked, otherwise nullopt.
    std::optional<ClassId> Update(const Input& input, uint32_t width, uint32_t height);

    // Draws the screen as overlay geometry, in pixels, the way the HUD is drawn
    // — no depth, after the post chain, under the text. That matters for the
    // one of these that goes over a live arena: cards submitted into the scene
    // would be sorted against the world's depth (a hill in front of a class
    // card) and would take the death screen's grey with them.
    //
    // Takes the bindings because the lines around the cards name the keys
    // they're talking about, and the player's are the only ones worth naming.
    void Render(Renderer& renderer, const Bindings& binds, Mode mode = Mode::Spawn);

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
    // One buffer rather than a pair, because overlay geometry has no depth to
    // sort by: submission order is draw order, so a card's outline is appended
    // straight after its fill instead of being kept apart in a second list.
    std::vector<Vertex> m_tris; // reused per frame
};
