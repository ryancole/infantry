#pragma once

#include <cstring>

// Polled input state, fed by the window procedure in main.cpp. Presses are
// latched as events (set on the down message, cleared at end of frame) so a
// click or tap shorter than one frame is never dropped.
struct Input
{
    bool  keys[256] = {};
    bool  keyPressEvents[256] = {};
    bool  mouseDown[3] = {};     // 0 = left, 1 = right, 2 = middle
    bool  mousePressEvents[3] = {};
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float wheel = 0.0f;          // accumulated notches this frame

    bool Key(int vk) const { return keys[vk & 0xFF]; }
    bool KeyPressed(int vk) const { return keyPressEvents[vk & 0xFF]; }
    bool MousePressed(int button) const { return mousePressEvents[button]; }

    // Call once at the end of each frame.
    void NextFrame()
    {
        std::memset(keyPressEvents, 0, sizeof(keyPressEvents));
        std::memset(mousePressEvents, 0, sizeof(mousePressEvents));
        wheel = 0.0f;
    }
};
