#pragma once

#include <DirectXMath.h>
#include <cstdint>

// Fixed-angle isometric camera (45° yaw, classic ~35.26° isometric pitch)
// with an orthographic projection and smoothed target following.
class IsoCamera
{
public:
    void SetViewport(uint32_t width, uint32_t height);
    void SetTarget(const DirectX::XMFLOAT3& target) { m_target = target; }
    void SnapToTarget() { m_focus = m_target; }
    void Update(float dt);

    // Scroll-wheel zoom: positive notches zoom in, negative zoom out.
    void AddZoom(float notches);

    DirectX::XMMATRIX ViewProj() const;

    uint32_t ViewportWidth() const { return m_width; }
    uint32_t ViewportHeight() const { return m_height; }

    // Projects a screen-space point onto the y = 0 ground plane.
    DirectX::XMFLOAT3 ScreenToGround(float screenX, float screenY) const;

    // Ground-plane basis vectors for screen-relative movement.
    DirectX::XMFLOAT3 ScreenUpOnGround() const;
    DirectX::XMFLOAT3 ScreenRightOnGround() const;

private:
    void ComputeMatrices(DirectX::XMMATRIX& view, DirectX::XMMATRIX& proj) const;

    DirectX::XMFLOAT3 m_focus = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_target = { 0.0f, 0.0f, 0.0f };
    float m_zoom = 26.0f;       // visible world units across the viewport width
    float m_zoomTarget = 26.0f;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;
};
