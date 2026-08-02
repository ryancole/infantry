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
        ThighL,
        ThighR,
        ShinL,
        ShinR,
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
    // legs — thighs from the hip, shins flexing at the knee behind them — and
    // `moveBlend` (0..1) eases the whole motion in and out.
    void Pose(DirectX::XMMATRIX out[SegmentCount], float walkPhase, float moveBlend);

    // Places a soldier's local space into the world at `pos`, facing `aimDir`.
    DirectX::XMMATRIX Base(const DirectX::SimpleMath::Vector3& pos,
                           const DirectX::SimpleMath::Vector3& aimDir);

    // Draws the model with each segment placed by its world matrix: space armor
    // over a dark undersuit, a helmet with a glowing visor, backpack + antenna,
    // and a rifle held two-handed.
    //
    // Two colors, because a soldier is two facts and they aren't equally
    // urgent. `team` paints the armor — chest, pauldrons, pack, pelvis, boots —
    // so which side someone is on is the shape you see from across the arena and
    // through the fog. `cls` paints the helmet alone, which is a small mark you
    // read once you're already looking at them, which is exactly when their
    // class starts to matter. Getting that order wrong is what made a field of
    // four class colors unreadable: it answered "what is that" everywhere and
    // "whose is it" nowhere.
    void Draw(Renderer& renderer, const DirectX::XMMATRIX world[SegmentCount],
              const DirectX::XMFLOAT4& team, const DirectX::XMFLOAT4& cls);
}
