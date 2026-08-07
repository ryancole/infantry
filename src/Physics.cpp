#include "Physics.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace
{
    // Object layers: what can collide with what at the narrow-phase level.
    namespace ObjLayers
    {
        constexpr JPH::ObjectLayer NON_MOVING = 0;
        constexpr JPH::ObjectLayer MOVING = 1;
        // Corpse ragdolls: they fall through the world's static geometry and
        // meet nothing else. Keeping them off MOVING means a body can never
        // stop a shot or shove a soldier, and — since a ragdoll's own limbs
        // overlap at every joint — spares us collision groups to stop a corpse
        // tearing itself apart.
        constexpr JPH::ObjectLayer DEBRIS = 2;
        constexpr JPH::ObjectLayer COUNT = 3;
    }

    // Broad-phase layers: statics live in their own tree so the (large, rarely
    // rebuilt) world geometry doesn't churn with the dynamic bodies.
    namespace BPLayers
    {
        constexpr JPH::BroadPhaseLayer NON_MOVING(0);
        constexpr JPH::BroadPhaseLayer MOVING(1);
        constexpr JPH::uint COUNT = 2;
    }

    class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::COUNT; }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            return layer == ObjLayers::NON_MOVING ? BPLayers::NON_MOVING : BPLayers::MOVING;
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
        {
            return layer == BPLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
        }
#endif
    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override
        {
            // Debris is only ever tested against the world's static tree.
            if (layer == ObjLayers::DEBRIS)
                return bpLayer == BPLayers::NON_MOVING;
            // Statics never collide with each other.
            return layer != ObjLayers::NON_MOVING || bpLayer != BPLayers::NON_MOVING;
        }
    };

    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
            if (a == ObjLayers::DEBRIS || b == ObjLayers::DEBRIS)
                return a == ObjLayers::NON_MOVING || b == ObjLayers::NON_MOVING;
            return a != ObjLayers::NON_MOVING || b != ObjLayers::NON_MOVING;
        }
    };

    // Records which dynamic bodies touched anything during a step. Jolt calls
    // this from its worker threads, hence the mutex.
    class ContactRecorder final : public JPH::ContactListener
    {
    public:
        void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                            const JPH::ContactManifold&, JPH::ContactSettings&) override
        {
            std::lock_guard lock(m_mutex);
            if (a.IsDynamic())
                m_hits.insert(a.GetID().GetIndexAndSequenceNumber());
            if (b.IsDynamic())
                m_hits.insert(b.GetID().GetIndexAndSequenceNumber());
        }

        void Clear()
        {
            std::lock_guard lock(m_mutex);
            m_hits.clear();
        }

        bool Contains(uint32_t handle) const
        {
            std::lock_guard lock(m_mutex);
            return m_hits.contains(handle);
        }

    private:
        mutable std::mutex m_mutex;
        std::unordered_set<uint32_t> m_hits;
    };

    // Process-wide Jolt setup (allocator, RTTI factory, shape registry).
    void InitJoltOnce()
    {
        static const bool once = [] {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
            return true;
        }();
        (void)once;
    }

    constexpr float kFixedStep = 1.0f / 60.0f;
    constexpr JPH::uint kMaxBodies = 4096;
    constexpr JPH::uint kMaxBodyPairs = 4096;
    constexpr JPH::uint kMaxContactConstraints = 2048;
}

struct Physics::Impl
{
    // A live constraint, remembered with the bodies it ties together so
    // RemoveBody can drop it before either end is destroyed.
    struct Joint
    {
        JPH::Ref<JPH::Constraint> constraint;
        uint32_t a;
        uint32_t b;
    };

    JPH::TempAllocatorImpl tempAllocator{ 10 * 1024 * 1024 };
    JPH::JobSystemThreadPool jobSystem{ JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                        static_cast<int>(std::max(1u, std::thread::hardware_concurrency() - 1)) };
    BPLayerInterfaceImpl bpLayers;
    ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
    ObjectLayerPairFilterImpl objPairFilter;
    JPH::PhysicsSystem system;
    ContactRecorder contacts;
    std::vector<Joint> joints;
    float accumulator = 0.0f;
};

Physics::Physics()
{
    InitJoltOnce();
    m_impl = std::make_unique<Impl>();
    m_impl->system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContactConstraints,
                        m_impl->bpLayers, m_impl->objVsBpFilter, m_impl->objPairFilter);
    m_impl->system.SetContactListener(&m_impl->contacts);
}

Physics::~Physics() = default;

void Physics::AddStaticBox(const XMFLOAT3& center, const XMFLOAT3& size)
{
    JPH::BodyCreationSettings settings(
        new JPH::BoxShape(JPH::Vec3(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f)),
        JPH::RVec3(center.x, center.y, center.z), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, ObjLayers::NON_MOVING);
    m_impl->system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
}

Physics::BodyHandle Physics::SpawnProjectile(const XMFLOAT3& pos, const XMFLOAT3& vel,
                                             float radius, float mass, float restitution,
                                             float gravityFactor)
{
    JPH::BodyCreationSettings settings(
        new JPH::SphereShape(radius),
        JPH::RVec3(pos.x, pos.y, pos.z), JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic, ObjLayers::MOVING);
    settings.mLinearVelocity = JPH::Vec3(vel.x, vel.y, vel.z);
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    // Shots that die on their first contact (see HadContact) pass 0 here, so
    // they never visibly rebound during the impact frame; a live grenade passes
    // a real restitution and keeps ricocheting until its fuse ends it.
    settings.mRestitution = restitution;
    settings.mFriction = 0.4f;
    // 0 for a bullet, which flies dead level until its fuse ends it. The drop
    // used to be what stopped a round, and that made reach a consequence of
    // gravity rather than a property of the weapon — see WeaponReach and the
    // reach paragraphs in PlayerClass.h.
    settings.mGravityFactor = gravityFactor;
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;

    const JPH::BodyID id = m_impl->system.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);
    return id.GetIndexAndSequenceNumber();
}

Physics::BodyHandle Physics::SpawnDebrisBox(const XMFLOAT3& center, const XMFLOAT3& size,
                                            const XMFLOAT4& rot, const XMFLOAT3& vel,
                                            const XMFLOAT3& angVel, float mass)
{
    const JPH::Vec3 half(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f);
    // Limb boxes are far smaller than the default convex radius, which has to
    // fit inside the box it rounds off.
    const float convexRadius = std::min(0.02f, half.ReduceMin() * 0.5f);

    JPH::BodyCreationSettings settings(
        new JPH::BoxShape(half, convexRadius), JPH::RVec3(center.x, center.y, center.z),
        JPH::Quat(rot.x, rot.y, rot.z, rot.w).Normalized(), JPH::EMotionType::Dynamic,
        ObjLayers::DEBRIS);
    settings.mLinearVelocity = JPH::Vec3(vel.x, vel.y, vel.z);
    settings.mAngularVelocity = JPH::Vec3(angVel.x, angVel.y, angVel.z);
    // Dead weight: high friction and almost no bounce, so a body drops where
    // it lands and stops instead of skating or skipping across the floor.
    settings.mFriction = 0.8f;
    settings.mRestitution = 0.05f;
    settings.mLinearDamping = 0.15f;
    settings.mAngularDamping = 0.4f;
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;

    const JPH::BodyID id = m_impl->system.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);
    return id.GetIndexAndSequenceNumber();
}

void Physics::AddConeJoint(BodyHandle parent, BodyHandle child, const XMFLOAT3& anchor,
                           const XMFLOAT3& boneAxis, float coneAngle, float twistAngle)
{
    JPH::Vec3 twist(boneAxis.x, boneAxis.y, boneAxis.z);
    if (twist.IsNearZero())
        twist = JPH::Vec3::sAxisY();
    twist = twist.Normalized();
    // Any axis square to the bone will do — the cone is symmetric about it, so
    // it only fixes where the twist angle is measured from.
    const JPH::Vec3 seed =
        std::abs(twist.GetY()) > 0.9f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
    const JPH::Vec3 plane = twist.Cross(seed).Normalized();

    // Both bodies get the same frame, so the pose the ragdoll is built in is
    // the middle of every joint's range: a corpse sags away from how it stood.
    JPH::SwingTwistConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mPosition1 = settings.mPosition2 = JPH::RVec3(anchor.x, anchor.y, anchor.z);
    settings.mTwistAxis1 = settings.mTwistAxis2 = twist;
    settings.mPlaneAxis1 = settings.mPlaneAxis2 = plane;
    settings.mNormalHalfConeAngle = coneAngle;
    settings.mPlaneHalfConeAngle = coneAngle;
    settings.mTwistMinAngle = -twistAngle;
    settings.mTwistMaxAngle = twistAngle;

    JPH::TwoBodyConstraint* constraint = m_impl->system.GetBodyInterface().CreateConstraint(
        &settings, JPH::BodyID(parent), JPH::BodyID(child));
    if (constraint == nullptr)
        return;
    m_impl->system.AddConstraint(constraint);
    m_impl->joints.push_back({ constraint, parent, child });
}

void Physics::RemoveBody(BodyHandle handle)
{
    // Any joint hanging off this body goes first: a constraint may not outlive
    // the bodies it holds.
    std::erase_if(m_impl->joints, [&](const Impl::Joint& joint) {
        if (joint.a != handle && joint.b != handle)
            return false;
        m_impl->system.RemoveConstraint(joint.constraint);
        return true;
    });

    const JPH::BodyID id(handle);
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
}

XMFLOAT3 Physics::GetPosition(BodyHandle handle) const
{
    const JPH::RVec3 p = m_impl->system.GetBodyInterface().GetPosition(JPH::BodyID(handle));
    return { p.GetX(), p.GetY(), p.GetZ() };
}

Physics::Transform Physics::GetTransform(BodyHandle handle) const
{
    JPH::RVec3 pos;
    JPH::Quat rot;
    m_impl->system.GetBodyInterface().GetPositionAndRotation(JPH::BodyID(handle), pos, rot);
    return { { pos.GetX(), pos.GetY(), pos.GetZ() },
             { rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW() } };
}

bool Physics::HadContact(BodyHandle handle) const
{
    return m_impl->contacts.Contains(handle);
}

void Physics::Step(float dt)
{
    m_impl->contacts.Clear();
    // Cap the backlog so a long stall doesn't trigger a catch-up death spiral.
    m_impl->accumulator = std::min(m_impl->accumulator + dt, 0.25f);
    while (m_impl->accumulator >= kFixedStep)
    {
        m_impl->system.Update(kFixedStep, 1, &m_impl->tempAllocator, &m_impl->jobSystem);
        m_impl->accumulator -= kFixedStep;
    }
}
