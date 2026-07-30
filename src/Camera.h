#pragma once

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <cstdint>

// Isometric camera (classic ~35.26° pitch, orbitable yaw starting at 45°)
// with an orthographic projection and smoothed target following.
class IsoCamera
{
public:
    void SetViewport(uint32_t width, uint32_t height);
    void SetTarget(const DirectX::SimpleMath::Vector3& target) { m_target = target; }
    void SnapToTarget() { m_focus = m_target; }
    void Update(float dt);

    // Scroll-wheel zoom: positive notches zoom in, negative zoom out.
    void AddZoom(float notches);

    // Orbits around the focus point (middle-mouse drag).
    void AddYaw(float radians) { m_yaw += radians; }

    DirectX::XMMATRIX ViewProj() const;

    uint32_t ViewportWidth() const { return m_width; }
    uint32_t ViewportHeight() const { return m_height; }

    // Projects a screen-space point onto the y = 0 ground plane.
    DirectX::SimpleMath::Vector3 ScreenToGround(float screenX, float screenY) const;

    // Ground-plane basis vectors for screen-relative movement.
    DirectX::SimpleMath::Vector3 ScreenUpOnGround() const;
    DirectX::SimpleMath::Vector3 ScreenRightOnGround() const;

private:
    void ComputeMatrices(DirectX::XMMATRIX& view, DirectX::XMMATRIX& proj) const;

    DirectX::SimpleMath::Vector3 m_focus;
    DirectX::SimpleMath::Vector3 m_target;
    float m_yaw = DirectX::XM_PIDIV4;
    float m_zoom = 26.0f;       // visible world units across the viewport width
    float m_zoomTarget = 26.0f;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;
};
