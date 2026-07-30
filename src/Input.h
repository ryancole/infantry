#pragma once

// windows.h must precede the toolkit headers: their Win32 message-pump API
// (ProcessMessage/SetWindow) is only declared when WM_USER is defined.
#include <windows.h>

#include <GamePad.h>
#include <Keyboard.h>
#include <Mouse.h>

// Per-frame input snapshot over the DirectXTK12 Keyboard/Mouse/GamePad
// singletons, which main.cpp's window procedure feeds via ProcessMessage.
// The toolkit trackers turn held state into edge events (the latching this
// struct used to do by hand), and are safe against lost focus: the toolkit
// resets state on WM_ACTIVATEAPP so keys can't stick.
struct Input
{
    DirectX::Keyboard::State kb = {};
    DirectX::Keyboard::KeyboardStateTracker kbEvents;
    DirectX::Mouse::State mouse = {};
    DirectX::Mouse::ButtonStateTracker mouseEvents;
    DirectX::GamePad::State pad = {};
    DirectX::GamePad::ButtonStateTracker padEvents;

    // Last absolute cursor position; frozen while the mouse is in relative
    // (camera-orbit) mode so aim doesn't jump.
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float wheel = 0.0f; // scroll notches this frame

    bool Key(int vk) const
    {
        return kb.IsKeyDown(static_cast<DirectX::Keyboard::Keys>(vk));
    }

    bool KeyPressed(int vk) const
    {
        return kbEvents.IsKeyPressed(static_cast<DirectX::Keyboard::Keys>(vk));
    }

    bool MouseDown(int button) const // 0 = left, 1 = right, 2 = middle
    {
        return button == 0 ? mouse.leftButton
             : button == 1 ? mouse.rightButton
                           : mouse.middleButton;
    }

    bool MousePressed(int button) const
    {
        using Tracker = DirectX::Mouse::ButtonStateTracker;
        const Tracker::ButtonState state = button == 0 ? mouseEvents.leftButton
                                         : button == 1 ? mouseEvents.rightButton
                                                       : mouseEvents.middleButton;
        return state == Tracker::PRESSED;
    }

    // Call once per frame, after the message pump.
    void Update()
    {
        using namespace DirectX;

        kb = Keyboard::Get().GetState();
        kbEvents.Update(kb);

        mouse = Mouse::Get().GetState();
        mouseEvents.Update(mouse);
        if (mouse.positionMode == Mouse::MODE_ABSOLUTE)
        {
            mouseX = static_cast<float>(mouse.x);
            mouseY = static_cast<float>(mouse.y);
        }
        wheel = static_cast<float>(mouse.scrollWheelValue) / 120.0f; // WHEEL_DELTA
        Mouse::Get().ResetScrollWheelValue();

        pad = GamePad::Get().GetState(0, GamePad::DEAD_ZONE_CIRCULAR);
        if (pad.IsConnected())
            padEvents.Update(pad);
        else
            padEvents.Reset();
    }
};
