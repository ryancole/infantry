#include "Soldier.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
    // Rest-pose landmarks in the soldier's local space (facing +Z, feet at
    // y = 0). Segment frames sit at the center of the part group they carry,
    // which is also where that segment's ragdoll box goes.
    constexpr float kLegLen = 0.43f; // hip to ankle
    constexpr float kHipX = 0.10f, kHipY = 0.50f, kHipZ = 0.02f;
    constexpr float kShoulderX = 0.25f, kShoulderY = 0.88f, kShoulderZ = 0.02f;
    constexpr float kPelvisY = 0.55f;
    constexpr float kTorsoY = 0.80f, kTorsoZ = -0.03f;
    constexpr float kHeadY = 1.05f, kHeadZ = 0.01f;
    constexpr float kSpineY = 0.66f; // waist joint, between pelvis and torso
    constexpr float kNeckY = 0.96f, kNeckZ = 0.01f;

    // Where the hands hold the rifle: right on the trigger, left crossed over
    // to the forestock. The arms are bones from the pauldrons to these, so the
    // two come out different lengths.
    const Vector3 kGripR(0.12f, 0.74f, 0.20f);
    const Vector3 kGripL(0.10f, 0.77f, 0.38f);

    // Walk cycle: the legs stride along the facing axis, and the torso dips a
    // touch at full stride spread so the walk reads even from the isometric
    // camera.
    constexpr float kSwingAngle = 0.6f;
    constexpr float kBobHeight = 0.03f;

    // Rotation taking +Y to `dir` (unit length). A segment's frame is oriented
    // along its bone, which is what lets a limb be one stretched cylinder.
    XMMATRIX AlignY(const Vector3& dir)
    {
        const float dot = std::clamp(dir.y, -1.0f, 1.0f);
        Vector3 axis = Vector3::UnitY.Cross(dir);
        if (axis.LengthSquared() < 1e-8f)
            return dot > 0.0f ? XMMatrixIdentity() : XMMatrixRotationX(XM_PI);
        axis.Normalize();
        return XMMatrixRotationAxis(XMLoadFloat3(&axis), std::acos(dot));
    }

    Vector3 Shoulder(float side, float bob)
    {
        return { side * kShoulderX, kShoulderY + bob, kShoulderZ };
    }

    Vector3 Hand(float side, float bob)
    {
        return (side < 0.0f ? kGripL : kGripR) + Vector3(0.0f, bob, 0.0f);
    }

    // Length of the shoulder-to-hand bone; the arm's cylinder and the hand at
    // its far end are both sized from it.
    float ArmLength(float side)
    {
        return (Shoulder(side, 0.0f) - Hand(side, 0.0f)).Length();
    }

    // Frame of one arm: origin at the middle of the bone, +Y running back up
    // to the shoulder.
    XMMATRIX ArmFrame(float side, float bob)
    {
        const Vector3 shoulder = Shoulder(side, bob);
        const Vector3 hand = Hand(side, bob);
        const Vector3 mid = (shoulder + hand) * 0.5f;
        Vector3 bone = shoulder - hand;
        bone.Normalize();
        return AlignY(bone) * XMMatrixTranslation(mid.x, mid.y, mid.z);
    }

    // The rifle is carried in the right hand, so it rides that arm's segment.
    // Its placement is authored in the soldier's local space (which reads far
    // better than hand-fitting it to a tilted bone), and rebased into the arm's
    // frame once here. Both the arm frame and the weapon shift with `bob`
    // identically, so rebasing at rest holds for every pose.
    const XMMATRIX& RifleRebase()
    {
        static const XMMATRIX inverseArm = XMMatrixInverse(nullptr, ArmFrame(1.0f, 0.0f));
        return inverseArm;
    }
}

// Boxes are a little generous around their parts: they only ever collide with
// the level, so a slight overlap between neighbors costs nothing and keeps a
// corpse from resting on a limb that visually clears the floor. Masses are
// roughly human proportions, summing to ~78.
const Soldier::Body Soldier::kBodies[Soldier::SegmentCount] = {
    /* Pelvis */ { { 0.32f, 0.20f, 0.24f }, 12.0f },
    /* Torso  */ { { 0.38f, 0.36f, 0.42f }, 30.0f },
    /* Head   */ { { 0.28f, 0.28f, 0.30f }, 7.0f },
    /* LegL   */ { { 0.17f, 0.55f, 0.24f }, 11.0f },
    /* LegR   */ { { 0.17f, 0.55f, 0.24f }, 11.0f },
    /* ArmL   */ { { 0.13f, 0.55f, 0.13f }, 4.0f },
    /* ArmR   */ { { 0.13f, 0.30f, 0.13f }, 3.0f },
};

// Anchors are in the parent's frame: subtract the parent's rest-pose center
// from the joint's position in the soldier's local space. Limits are loose
// enough to fold and flop, tight enough that nothing bends inside-out.
const Soldier::Joint Soldier::kJoints[Soldier::kJointCount] = {
    { Pelvis, Torso, { 0.0f, kSpineY - kPelvisY, 0.0f },
      XMConvertToRadians(35.0f), XMConvertToRadians(30.0f) },
    { Torso, Head, { 0.0f, kNeckY - kTorsoY, kNeckZ - kTorsoZ },
      XMConvertToRadians(40.0f), XMConvertToRadians(45.0f) },
    { Pelvis, LegL, { -kHipX, kHipY - kPelvisY, kHipZ },
      XMConvertToRadians(60.0f), XMConvertToRadians(25.0f) },
    { Pelvis, LegR, { kHipX, kHipY - kPelvisY, kHipZ },
      XMConvertToRadians(60.0f), XMConvertToRadians(25.0f) },
    { Torso, ArmL, { -kShoulderX, kShoulderY - kTorsoY, kShoulderZ - kTorsoZ },
      XMConvertToRadians(80.0f), XMConvertToRadians(60.0f) },
    { Torso, ArmR, { kShoulderX, kShoulderY - kTorsoY, kShoulderZ - kTorsoZ },
      XMConvertToRadians(80.0f), XMConvertToRadians(60.0f) },
};

void Soldier::Pose(XMMATRIX out[SegmentCount], float walkPhase, float moveBlend)
{
    const float swing = std::sin(walkPhase) * kSwingAngle * moveBlend;
    const float bob = kBobHeight * moveBlend * std::cos(walkPhase * 2.0f);

    out[Pelvis] = XMMatrixTranslation(0.0f, kPelvisY + bob, 0.0f);
    out[Torso] = XMMatrixTranslation(0.0f, kTorsoY + bob, kTorsoZ);
    out[Head] = XMMatrixTranslation(0.0f, kHeadY + bob, kHeadZ);

    for (int i = 0; i < 2; ++i)
    {
        const float side = (i == 0) ? -1.0f : 1.0f;
        // The leg pivots about its hip. Its frame's +Y runs up the bone, so it
        // sits half a leg below the joint and rotates around it; the boot,
        // authored at the bottom of that frame, follows the stride with it.
        out[LegL + i] = XMMatrixTranslation(0.0f, -kLegLen * 0.5f, 0.0f) *
                        XMMatrixRotationX(-swing * side) *
                        XMMatrixTranslation(side * kHipX, kHipY, kHipZ);
        out[ArmL + i] = ArmFrame(side, bob);
    }
}

XMMATRIX Soldier::Base(const Vector3& pos, const Vector3& aimDir)
{
    return XMMatrixRotationY(std::atan2(aimDir.x, aimDir.z)) *
           XMMatrixTranslation(pos.x, pos.y, pos.z);
}

void Soldier::Draw(Renderer& renderer, const XMMATRIX world[SegmentCount], const XMFLOAT4& team,
                   const XMFLOAT4& cls)
{
    const XMFLOAT4 plate = team;
    const XMFLOAT4 plateDark = { team.x * 0.55f, team.y * 0.55f, team.z * 0.55f, 1.0f };
    const XMFLOAT4 suit = { 0.15f, 0.16f, 0.19f, 1.0f };
    const XMFLOAT4 metal = { 0.09f, 0.10f, 0.12f, 1.0f };
    const XMFLOAT4 visor = { 0.35f, 0.95f, 1.00f, 1.0f };

    // Each part is authored in its segment's frame, so posing the segment
    // carries the whole group with it.
    auto part = [&](Segment seg, Shape shape, const XMMATRIX& local, const XMFLOAT4& col) {
        renderer.DrawShape(shape, local * world[seg], col);
    };

    for (int i = 0; i < 2; ++i) // legs: suit cylinder down the bone, boot at the ankle
    {
        const Segment leg = static_cast<Segment>(LegL + i);
        part(leg, Shape::CylinderLow, XMMatrixScaling(0.13f, kLegLen, 0.13f), suit);
        part(leg, Shape::Box,
             XMMatrixScaling(0.15f, 0.10f, 0.24f) *
                 XMMatrixTranslation(0.0f, -kLegLen * 0.5f - 0.02f, 0.05f),
             plateDark);
    }

    part(Pelvis, Shape::Box, XMMatrixScaling(0.30f, 0.16f, 0.22f), plateDark);

    part(Torso, Shape::Box, // undersuit torso, mostly hidden by the chest plate
         XMMatrixScaling(0.26f, 0.30f, 0.18f) * XMMatrixTranslation(0.0f, -0.08f, 0.03f), suit);
    part(Torso, Shape::Box, // chest plate
         XMMatrixScaling(0.36f, 0.26f, 0.28f) * XMMatrixTranslation(0.0f, -0.01f, 0.04f), plate);
    part(Torso, Shape::Box, // backpack / life support
         XMMatrixScaling(0.26f, 0.30f, 0.14f) * XMMatrixTranslation(0.0f, 0.0f, -0.18f), plateDark);
    part(Torso, Shape::CylinderLow, // antenna
         XMMatrixScaling(0.025f, 0.30f, 0.025f) * XMMatrixTranslation(-0.09f, 0.22f, -0.19f),
         metal);
    part(Torso, Shape::SphereLow, // antenna tip, catches the bloom like the visor
         XMMatrixScaling(0.05f, 0.05f, 0.05f) * XMMatrixTranslation(-0.09f, 0.38f, -0.19f), visor);
    for (float side : { -1.0f, 1.0f }) // shoulder pauldrons, capping the arm joints
        part(Torso, Shape::SphereMed,
             XMMatrixScaling(0.19f, 0.19f, 0.19f) *
                 XMMatrixTranslation(side * kShoulderX, 0.10f, 0.04f),
             plate);
    part(Torso, Shape::CylinderLow, // neck seal
         XMMatrixScaling(0.11f, 0.08f, 0.11f) * XMMatrixTranslation(0.0f, 0.15f, 0.04f), suit);

    // The helmet is the class mark: the one piece of a soldier that isn't
    // painted for their side. It's small — a quarter of a body — but it sits at
    // the top of the model, which under an isometric camera is the part least
    // likely to be behind anything, and it's the part a player is already
    // looking at when they line up a shot.
    part(Head, Shape::SphereMed, XMMatrixScaling(0.27f, 0.27f, 0.27f), cls);
    part(Head, Shape::SphereLow, // visor, bulging out of the helmet's front
         XMMatrixScaling(0.17f, 0.10f, 0.12f) * XMMatrixTranslation(0.0f, 0.0f, 0.09f), visor);

    for (int i = 0; i < 2; ++i) // arms: pauldron to hand, gloved fist at the end
    {
        const Segment arm = static_cast<Segment>(ArmL + i);
        const float len = ArmLength((i == 0) ? -1.0f : 1.0f);
        part(arm, Shape::CylinderLow, XMMatrixScaling(0.10f, len, 0.10f), suit);
        part(arm, Shape::SphereLow,
             XMMatrixScaling(0.11f, 0.11f, 0.11f) * XMMatrixTranslation(0.0f, -len * 0.5f, 0.0f),
             metal);
    }

    part(ArmR, Shape::Box, // rifle body
         XMMatrixScaling(0.07f, 0.10f, 0.42f) * XMMatrixTranslation(0.11f, 0.77f, 0.26f) *
             RifleRebase(),
         metal);
    part(ArmR, Shape::CylinderLow, // barrel, ending near the muzzle spawn point
         XMMatrixScaling(0.045f, 0.28f, 0.045f) * XMMatrixRotationX(XM_PIDIV2) *
             XMMatrixTranslation(0.11f, 0.79f, 0.58f) * RifleRebase(),
         metal);
}
