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
    // No body at all — matches Jolt's own invalid id, and deliberately not 0,
    // which is a raw value the first body created can legitimately own. A
    // client replica's projectiles carry this: they have positions, not
    // bodies.
    static constexpr BodyHandle kInvalidBody = 0xffffffffu;

    Physics();
    ~Physics();

    // Static world collision (floor, bunkers). size is the full extent.
    void AddStaticBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& size);

    // Dynamic sphere; uses continuous collision so fast shots don't tunnel
    // through thin geometry. `restitution` is the fraction of impact speed a
    // bounce keeps: 0 stops the body dead where it lands (shots that die on
    // their first contact), while a grenade rebounds off walls and floor until
    // its fuse runs out.
    //
    // `gravityFactor` is how much of the world's gravity the body feels, and 0
    // is the ordinary case rather than the exotic one: a bullet flies level and
    // is ended by its own fuse, so how far it reaches is a number the weapon
    // states rather than a thing the floor decides. Only what is thrown or
    // lobbed passes 1 and arcs.
    BodyHandle SpawnProjectile(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& vel,
                               float radius, float mass, float restitution = 0.0f,
                               float gravityFactor = 1.0f);

    // A corpse limb: a dynamic box that collides with the static world and
    // nothing else, so a ragdoll can't stop a bullet, shove a soldier, or have
    // its own limbs fight each other. `rot` is a quaternion (x, y, z, w).
    BodyHandle SpawnDebrisBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& size,
                              const DirectX::XMFLOAT4& rot, const DirectX::XMFLOAT3& vel,
                              const DirectX::XMFLOAT3& angVel, float mass);

    // Cone-and-twist joint pinning two bodies together at the world-space
    // point `anchor`. `boneAxis` is the limb's rest direction; `coneAngle`
    // caps how far off it the joint may swing and `twistAngle` how far it may
    // spin about it (both radians). Joints are dropped automatically when
    // either of their bodies is removed.
    void AddConeJoint(BodyHandle parent, BodyHandle child, const DirectX::XMFLOAT3& anchor,
                      const DirectX::XMFLOAT3& boneAxis, float coneAngle, float twistAngle);

    void RemoveBody(BodyHandle handle);
    DirectX::XMFLOAT3 GetPosition(BodyHandle handle) const;

    // Position and orientation of a body, for anything whose rotation is the
    // simulation's to decide (a tumbling corpse; a projectile is a sphere and
    // only needs GetPosition).
    struct Transform
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT4 rot; // quaternion (x, y, z, w)
    };
    Transform GetTransform(BodyHandle handle) const;

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
