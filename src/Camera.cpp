#include "Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    constexpr float kPitch = 0.61547971f;      // atan(1/sqrt(2)) ~ 35.26 degrees
    constexpr float kFollowRate = 8.0f;

    constexpr float kMinZoom = 10.0f;
    constexpr float kMaxZoom = 240.0f;
    constexpr float kZoomStep = 1.15f;         // multiplicative, per wheel notch
    constexpr float kZoomRate = 10.0f;
}

void IsoCamera::SetViewport(uint32_t width, uint32_t height)
{
    if (width > 0 && height > 0)
    {
        m_width = width;
        m_height = height;
    }
}

void IsoCamera::Update(float dt)
{
    // Exponential smoothing toward the target, framerate independent.
    m_focus = Vector3::Lerp(m_focus, m_target, 1.0f - std::exp(-kFollowRate * dt));
    m_zoom += (m_zoomTarget - m_zoom) * (1.0f - std::exp(-kZoomRate * dt));
}

void IsoCamera::AddZoom(float notches)
{
    m_zoomTarget = std::clamp(m_zoomTarget * std::pow(kZoomStep, -notches),
                              kMinZoom, kMaxZoom);
}

void IsoCamera::ComputeMatrices(XMMATRIX& view, XMMATRIX& proj) const
{
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float viewHeight = m_zoom / aspect;

    // Ground at the top/bottom screen edge sits this far in front of or behind
    // the focus along the view axis; the eye distance and far plane must both
    // exceed it (plus margin for tall geometry) or the ground clips at high zoom.
    const float depthReach = 0.5f * viewHeight / std::tan(kPitch);
    const float eyeDistance = depthReach + 60.0f;

    const XMVECTOR focus = XMVectorSet(m_focus.x, 0.0f, m_focus.z, 1.0f);
    const XMVECTOR toEye = XMVectorSet(std::sin(m_yaw) * std::cos(kPitch), std::sin(kPitch),
                                       std::cos(m_yaw) * std::cos(kPitch), 0.0f);
    const XMVECTOR eye = XMVectorAdd(focus, XMVectorScale(toEye, eyeDistance));
    view = XMMatrixLookAtLH(eye, focus, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

    proj = XMMatrixOrthographicLH(m_zoom, viewHeight, 0.1f, eyeDistance + depthReach + 60.0f);
}

XMMATRIX IsoCamera::ViewProj() const
{
    XMMATRIX view, proj;
    ComputeMatrices(view, proj);
    return view * proj;
}

Vector3 IsoCamera::ScreenToGround(float screenX, float screenY) const
{
    XMMATRIX view, proj;
    ComputeMatrices(view, proj);

    const Vector3 nearPt = XMVector3Unproject(
        XMVectorSet(screenX, screenY, 0.0f, 0.0f), 0.0f, 0.0f,
        static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f,
        proj, view, XMMatrixIdentity());
    const Vector3 farPt = XMVector3Unproject(
        XMVectorSet(screenX, screenY, 1.0f, 0.0f), 0.0f, 0.0f,
        static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f,
        proj, view, XMMatrixIdentity());

    Vector3 dir = farPt - nearPt;
    dir.Normalize();

    float dist = 0.0f;
    const Ray ray(nearPt, dir);
    if (!ray.Intersects(Plane(Vector3::UnitY, 0.0f), dist))
        return m_focus;

    Vector3 hit = ray.position + ray.direction * dist;
    hit.y = 0.0f;
    return hit;
}

Vector3 IsoCamera::ScreenUpOnGround() const
{
    // Camera looks along -toEye; projected onto the ground plane and
    // normalized, that is simply the negated yaw direction.
    return { -std::sin(m_yaw), 0.0f, -std::cos(m_yaw) };
}

Vector3 IsoCamera::ScreenRightOnGround() const
{
    return { -std::cos(m_yaw), 0.0f, std::sin(m_yaw) };
}

Vector3 IsoCamera::EyeDirection() const
{
    // The same toEye ComputeMatrices builds, and deliberately the same
    // expression rather than a second derivation of it: the day the pitch or
    // the orbit changes, a ray marched toward an eye that had moved somewhere
    // else would go on quietly answering the wrong question.
    return { std::sin(m_yaw) * std::cos(kPitch), std::sin(kPitch),
             std::cos(m_yaw) * std::cos(kPitch) };
}
