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
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_set>

using namespace DirectX;

namespace
{
    // Object layers: what can collide with what at the narrow-phase level.
    namespace ObjLayers
    {
        constexpr JPH::ObjectLayer NON_MOVING = 0;
        constexpr JPH::ObjectLayer MOVING = 1;
        constexpr JPH::ObjectLayer COUNT = 2;
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
            // Statics never collide with each other.
            return layer != ObjLayers::NON_MOVING || bpLayer != BPLayers::NON_MOVING;
        }
    };

    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
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
    JPH::TempAllocatorImpl tempAllocator{ 10 * 1024 * 1024 };
    JPH::JobSystemThreadPool jobSystem{ JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                        static_cast<int>(std::max(1u, std::thread::hardware_concurrency() - 1)) };
    BPLayerInterfaceImpl bpLayers;
    ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
    ObjectLayerPairFilterImpl objPairFilter;
    JPH::PhysicsSystem system;
    ContactRecorder contacts;
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
                                             float radius, float mass)
{
    JPH::BodyCreationSettings settings(
        new JPH::SphereShape(radius),
        JPH::RVec3(pos.x, pos.y, pos.z), JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic, ObjLayers::MOVING);
    settings.mLinearVelocity = JPH::Vec3(vel.x, vel.y, vel.z);
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    // Projectiles die on their first contact (see HadContact), so zero
    // restitution keeps them from visibly rebounding during the impact frame.
    settings.mRestitution = 0.0f;
    settings.mFriction = 0.4f;
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;

    const JPH::BodyID id = m_impl->system.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);
    return id.GetIndexAndSequenceNumber();
}

void Physics::RemoveBody(BodyHandle handle)
{
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
