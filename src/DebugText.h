#pragma once

#include "Renderer.h"

#include <string_view>
#include <vector>

// Tiny vector line font (uppercase letters, digits, and a little punctuation)
// for menus and debug overlays. Text is emitted as line-list vertices drawn
// with the same batches as everything else, so no font assets are needed.
// Unknown characters render as blank space.
namespace DebugText
{
    // Appends line-list vertices for `text` with its top-left corner at
    // (x, y), `size` pixels tall, at depth z (for screen-space ortho drawing
    // where smaller z is closer to the viewer).
    void Append(std::vector<Vertex>& out, std::string_view text, float x, float y,
                float size, const DirectX::XMFLOAT4& color, float z = 0.0f);

    // Width in pixels that Append would cover, for centering.
    float Measure(std::string_view text, float size);
}
