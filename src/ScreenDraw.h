#pragma once

#include "Renderer.h"

#include <DirectXMath.h>
#include <string_view>
#include <vector>

// The drawing every full-screen menu does. A menu is rectangles and centered
// words: the renderer offers neither, so each screen used to grow its own pair
// of them, and two screens agreeing on what a rectangle is by coincidence is
// one screen away from not agreeing. They live here instead, and a screen that
// needs something the others don't still writes that itself.
//
// These build into the caller's vertex buffers rather than drawing, because
// the batch is what makes a screen one draw call apiece: fills accumulate in
// one buffer, outlines in another, and the screen submits both when it's done.
// `z` is the ortho depth the screen sets up (smaller is nearer), so a screen
// can say what sits over what without ordering its own appends.
namespace ScreenDraw
{
    inline void AppendQuad(std::vector<Vertex>& out, float x, float y, float w, float h, float z,
                           const DirectX::XMFLOAT4& color)
    {
        const Vertex v[4] = {
            { DirectX::XMFLOAT3{ x, y, z }, color },
            { DirectX::XMFLOAT3{ x + w, y, z }, color },
            { DirectX::XMFLOAT3{ x + w, y + h, z }, color },
            { DirectX::XMFLOAT3{ x, y + h, z }, color },
        };
        out.push_back(v[0]);
        out.push_back(v[1]);
        out.push_back(v[2]);
        out.push_back(v[0]);
        out.push_back(v[2]);
        out.push_back(v[3]);
    }

    inline void AppendOutline(std::vector<Vertex>& out, float x, float y, float w, float h, float z,
                              const DirectX::XMFLOAT4& color)
    {
        const DirectX::XMFLOAT3 p[4] = {
            { x, y, z }, { x + w, y, z }, { x + w, y + h, z }, { x, y + h, z }
        };
        for (int i = 0; i < 4; ++i)
        {
            out.push_back({ p[i], color });
            out.push_back({ p[(i + 1) % 4], color });
        }
    }

    inline void DrawCentered(Renderer& renderer, std::string_view text, float centerX, float y,
                             float size, const DirectX::XMFLOAT4& color)
    {
        renderer.DrawScreenText(text, centerX - renderer.MeasureScreenText(text, size) * 0.5f, y,
                                size, color);
    }

    // A color at a fraction of its brightness, alpha untouched: how these
    // screens say "the same thing, but not the one you're looking at".
    inline DirectX::XMFLOAT4 Dim(const DirectX::XMFLOAT4& c, float f)
    {
        return { c.x * f, c.y * f, c.z * f, c.w };
    }
}
