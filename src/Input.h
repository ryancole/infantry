#pragma once

#include <cstring>

// Polled input state, fed by the window procedure in main.cpp.
struct Input
{
    bool  keys[256] = {};
    bool  prevKeys[256] = {};
    bool  mouseDown[3] = {};     // 0 = left, 1 = right, 2 = middle
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float wheel = 0.0f;          // accumulated notches this frame

    bool Key(int vk) const { return keys[vk & 0xFF]; }
    bool KeyPressed(int vk) const { return keys[vk & 0xFF] && !prevKeys[vk & 0xFF]; }

    // Call once at the end of each frame.
    void NextFrame()
    {
        std::memcpy(prevKeys, keys, sizeof(keys));
        wheel = 0.0f;
    }
};
