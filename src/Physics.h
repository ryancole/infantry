#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <memory>

// Thin wrapper around Jolt Physics: owns the physics world and exposes the
// small surface the game needs (static level geometry, dynamic projectiles).
// Jolt types stay out of this header so only Physics.cpp compiles against
// the SDK.
class Physics
{
public:
    // Opaque body handle (a JPH::BodyID's raw value).
    using BodyHandle = uint32_t;

    Physics();
    ~Physics();

    // Static world collision (floor, bunkers). size is the full extent.
    void AddStaticBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& size);

    // Dynamic sphere under gravity; uses continuous collision so fast shots
    // don't tunnel through thin geometry. Zero restitution: projectiles stop
    // dead where they land instead of bouncing.
    BodyHandle SpawnProjectile(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& vel,
                               float radius, float mass);

    void RemoveBody(BodyHandle handle);
    DirectX::XMFLOAT3 GetPosition(BodyHandle handle) const;

    // True if the body touched anything during the most recent Step. Used to
    // despawn projectiles on impact instead of letting them ricochet.
    bool HadContact(BodyHandle handle) const;

    // Advances the simulation on a fixed 60 Hz tick (accumulates dt so render
    // rate and simulation rate stay decoupled).
    void Step(float dt);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
