#pragma once

#include "Renderer.h"

#include <DirectXMath.h>
#include <SimpleMath.h>

// The soldier model, shared by the player, the NPCs, and the corpses they
// leave behind. It is a rigid assembly of the renderer's lit primitives, but
// grouped into segments rather than drawn through one transform: every part is
// authored in the frame of the segment it rides, so the same model can be posed
// from two very different sources. A living soldier's segments come from the
// walk cycle; a dead one's come straight from the ragdoll bodies of its corpse.
namespace Soldier
{
    enum Segment
    {
        Pelvis,
        Torso,
        Head,
        LegL,
        LegR,
        ArmL,
        ArmR,
        SegmentCount,
    };

    // The physical stand-in for a segment: a box centered on the segment's
    // frame, sized to cover the parts drawn on it, carrying its share of body
    // mass. Only corpses use these — living soldiers are kinematic.
    struct Body
    {
        DirectX::XMFLOAT3 size;
        float mass;
    };
    extern const Body kBodies[SegmentCount];

    // How a segment hangs off its parent. `anchor` is the pivot expressed in
    // the parent's frame, which keeps it independent of the pose. The cone is
    // centered on the child's own +Y — the axis running up the limb toward the
    // joint — so the model's rest pose sits in the middle of the joint's range.
    // Angles are in radians.
    struct Joint
    {
        Segment parent;
        Segment child;
        DirectX::XMFLOAT3 anchor;
        float coneAngle;  // how far off its bone the limb may swing
        float twistAngle; // how far it may spin about the bone, either way
    };
    constexpr int kJointCount = SegmentCount - 1; // a tree: one joint per child
    extern const Joint kJoints[kJointCount];

    // Fills `out` with the segment frames in the soldier's local space (facing
    // +Z, feet at y = 0) for a point in the walk cycle. `walkPhase` swings the
    // legs and `moveBlend` (0..1) eases the whole motion in and out.
    void Pose(DirectX::XMMATRIX out[SegmentCount], float walkPhase, float moveBlend);

    // Places a soldier's local space into the world at `pos`, facing `aimDir`.
    DirectX::XMMATRIX Base(const DirectX::SimpleMath::Vector3& pos,
                           const DirectX::SimpleMath::Vector3& aimDir);

    // Draws the model with each segment placed by its world matrix: space armor
    // in the class color over a dark undersuit, a helmet with a glowing visor,
    // backpack + antenna, and a rifle held two-handed.
    void Draw(Renderer& renderer, const DirectX::XMMATRIX world[SegmentCount],
              const DirectX::XMFLOAT4& color);
}
