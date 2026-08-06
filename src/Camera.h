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

    // Which way the eye lies from anywhere in the arena, as a unit vector. The
    // projection is orthographic, so this is one direction for the whole scene
    // rather than a different one per point — which is what makes "is there a
    // wall between the camera and that soldier" a single ray to march instead
    // of a question about where the eye happens to be. Handed out because the
    // answer to that question is nobody's but the caller's; the camera has no
    // opinion about what is worth occluding.
    DirectX::SimpleMath::Vector3 EyeDirection() const;

private:
    void ComputeMatrices(DirectX::XMMATRIX& view, DirectX::XMMATRIX& proj) const;

    DirectX::SimpleMath::Vector3 m_focus;
    DirectX::SimpleMath::Vector3 m_target;
    float m_yaw = DirectX::XM_PIDIV4;
    // Visible world units across the viewport width. Thirty-four rather than
    // the twenty-six it opened on for years, and the reason is that the width
    // was never the number that mattered: under this camera the ground the
    // screen actually holds is a rectangle of about Z by Z, so what a player
    // can see in any direction is half of it. At twenty-six that was under
    // thirteen units, and everything else in the game — how far a soldier
    // sees, how close one comes before it shoots, how far a shot carries —
    // was set against numbers two and three times that. The screen was the
    // smallest range in the game and nothing was measured against it.
    //
    // The ceiling on this is the fog: the shortest-sighted classes see 26
    // units (medic, grenadier), and once the corner of the screen reaches
    // further than that, a player watches their own sight end in a circle
    // painted inside the frame instead of on the walls. The corner sits at
    // about 0.70 * Z, so 26 / 0.70 puts the limit near thirty-seven and this
    // takes thirty-four, which leaves the shortest sight a couple of units
    // clear of the corner. Zooming out by hand still passes it — that has
    // always been true, and kMaxZoom is a player's business — but the view
    // the game opens on no longer does.
    float m_zoom = 34.0f;
    float m_zoomTarget = 34.0f;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;
};
