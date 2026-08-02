#pragma once

#include <DirectXMath.h>
#include <VertexTypes.h>

// Dynamic debug/gameplay geometry vertex (position + color). In its own
// header rather than the renderer's because it's the one type that crosses
// the line between describing geometry and drawing it: the ability marks and
// the HUD build vectors of these, and they should be able to say so without
// pulling in a device, a swap chain, and half of D3D12 to do it.
using Vertex = DirectX::VertexPositionColor;
