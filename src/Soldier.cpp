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
    // The leg is two bones so it can bend: hip to knee, knee to ankle. The
    // thigh is the longer of the two, as it is on a person.
    constexpr float kThighLen = 0.23f, kShinLen = 0.20f;
    constexpr float kHipX = 0.10f, kHipY = 0.50f, kHipZ = 0.02f;
    constexpr float kShoulderX = 0.25f, kShoulderY = 0.88f, kShoulderZ = 0.02f;
    constexpr float kPelvisY = 0.55f;
    constexpr float kTorsoY = 0.80f, kTorsoZ = -0.03f;
    constexpr float kHeadY = 1.05f, kHeadZ = 0.01f;
    constexpr float kSpineY = 0.66f; // waist joint, between pelvis and torso
    constexpr float kNeckY = 0.96f, kNeckZ = 0.01f;

    // Both arms are the same two bones — they belong to the same person — and
    // the difference between them is the fold at the elbow, not the reach.
    constexpr float kUpperArmLen = 0.28f, kForearmLen = 0.26f;

    // Where the hands hold the rifle: right on the trigger, left crossed over
    // to the forestock. These are the fixed ends the arms are solved back from,
    // and the shoulder-to-grip spans they imply differ by half: the trigger arm
    // is folded in tight against the body while the support arm is out near
    // full stretch, which is what holding a rifle actually looks like.
    const Vector3 kGripR(0.12f, 0.74f, 0.20f);
    const Vector3 kGripL(0.10f, 0.77f, 0.38f);

    // A shoulder and a grip that are both nailed down leave the elbow free to
    // spin on a circle between them, and nothing in the pose picks a point on
    // it. This hint does: out from the side, down, and back, which is where an
    // elbow goes. Mirrored by side.
    const Vector3 kElbowPole(0.6f, -0.5f, -0.6f);

    // Walk cycle: the legs stride along the facing axis, and the torso dips a
    // touch at full stride spread so the walk reads even from the isometric
    // camera. The knee folds only while a leg is swinging through — that's what
    // lifts the boot clear of the ground instead of dragging it along — and is
    // straight again by the time the foot plants.
    constexpr float kSwingAngle = 0.6f;
    constexpr float kKneeBend = 0.75f;
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

    // Frame of one bone: origin at its middle, +Y running back up toward
    // `top`, which is what lets the bone be drawn as one stretched cylinder.
    XMMATRIX BoneFrame(const Vector3& top, const Vector3& bottom)
    {
        const Vector3 mid = (top + bottom) * 0.5f;
        Vector3 bone = top - bottom;
        bone.Normalize();
        return AlignY(bone) * XMMatrixTranslation(mid.x, mid.y, mid.z);
    }

    // Two-bone IK: the elbow that puts an upper arm and a forearm of fixed
    // length between the shoulder and the grip. The legs get their pose handed
    // to them by the walk cycle, but an arm can't be posed that way — its far
    // end is pinned to the weapon, so the joint in the middle has to be solved
    // for rather than animated.
    Vector3 Elbow(float side, float bob)
    {
        const Vector3 shoulder = Shoulder(side, bob);
        Vector3 axis = Hand(side, bob) - shoulder;
        // Never let the grips ask for more reach than the arm has; a span past
        // full stretch has no elbow on it at all, only a square root of a
        // negative number.
        const float span = std::min(axis.Length(), kUpperArmLen + kForearmLen - 0.001f);
        axis.Normalize();

        // How far along the shoulder-to-grip line the elbow sits, and how far
        // off it — the two triangle sides that fall out of the bone lengths.
        const float along = (span * span + kUpperArmLen * kUpperArmLen -
                             kForearmLen * kForearmLen) / (2.0f * span);
        const float out = std::sqrt(std::max(0.0f, kUpperArmLen * kUpperArmLen - along * along));

        Vector3 pole(side * kElbowPole.x, kElbowPole.y, kElbowPole.z);
        pole -= axis * pole.Dot(axis); // only the part of the hint the arm can honor
        if (pole.LengthSquared() < 1e-8f)          // hint ran straight down the bone
            pole = Vector3::UnitY.Cross(axis);     // any perpendicular will do
        pole.Normalize();
        return shoulder + axis * along + pole * out;
    }

    XMMATRIX UpperArmFrame(float side, float bob)
    {
        return BoneFrame(Shoulder(side, bob), Elbow(side, bob));
    }

    XMMATRIX ForearmFrame(float side, float bob)
    {
        return BoneFrame(Elbow(side, bob), Hand(side, bob));
    }

    // The rifle is carried in the right hand, so it rides that forearm. Its
    // placement is authored in the soldier's local space (which reads far
    // better than hand-fitting it to a tilted bone), and rebased into the
    // forearm's frame once here. Both the forearm and the weapon shift with
    // `bob` identically, so rebasing at rest holds for every pose.
    const XMMATRIX& RifleRebase()
    {
        static const XMMATRIX inverseArm = XMMatrixInverse(nullptr, ForearmFrame(1.0f, 0.0f));
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
    /* ThighL */ { { 0.17f, 0.30f, 0.19f }, 7.0f },
    /* ThighR */ { { 0.17f, 0.30f, 0.19f }, 7.0f },
    /* ShinL  */ { { 0.15f, 0.28f, 0.24f }, 4.0f },
    /* ShinR  */ { { 0.15f, 0.28f, 0.24f }, 4.0f },
    /* UpArmL */ { { 0.14f, 0.32f, 0.14f }, 2.0f },
    /* UpArmR */ { { 0.14f, 0.32f, 0.14f }, 2.0f },
    /* ForeL  */ { { 0.12f, 0.30f, 0.12f }, 1.5f },
    /* ForeR  */ { { 0.12f, 0.30f, 0.12f }, 1.5f },
};

// Anchors are in the parent's frame: subtract the parent's rest-pose center
// from the joint's position in the soldier's local space. Limits are loose
// enough to fold and flop, tight enough that nothing bends inside-out.
const Soldier::Joint Soldier::kJoints[Soldier::kJointCount] = {
    { Pelvis, Torso, { 0.0f, kSpineY - kPelvisY, 0.0f },
      XMConvertToRadians(35.0f), XMConvertToRadians(30.0f) },
    { Torso, Head, { 0.0f, kNeckY - kTorsoY, kNeckZ - kTorsoZ },
      XMConvertToRadians(40.0f), XMConvertToRadians(45.0f) },
    { Pelvis, ThighL, { -kHipX, kHipY - kPelvisY, kHipZ },
      XMConvertToRadians(60.0f), XMConvertToRadians(25.0f) },
    { Pelvis, ThighR, { kHipX, kHipY - kPelvisY, kHipZ },
      XMConvertToRadians(60.0f), XMConvertToRadians(25.0f) },
    // A knee is a hinge, and a cone is the wrong shape for one — but a narrow
    // cone with almost no twist keeps the shin tracking its thigh instead of
    // splaying, which is the part you'd notice on a corpse.
    { ThighL, ShinL, { 0.0f, -kThighLen * 0.5f, 0.0f },
      XMConvertToRadians(25.0f), XMConvertToRadians(10.0f) },
    { ThighR, ShinR, { 0.0f, -kThighLen * 0.5f, 0.0f },
      XMConvertToRadians(25.0f), XMConvertToRadians(10.0f) },
    { Torso, UpperArmL, { -kShoulderX, kShoulderY - kTorsoY, kShoulderZ - kTorsoZ },
      XMConvertToRadians(80.0f), XMConvertToRadians(60.0f) },
    { Torso, UpperArmR, { kShoulderX, kShoulderY - kTorsoY, kShoulderZ - kTorsoZ },
      XMConvertToRadians(80.0f), XMConvertToRadians(60.0f) },
    // Elbows get more room than knees: an arm folds further, and a corpse's
    // forearm flopping wide reads as slack rather than as broken.
    { UpperArmL, ForearmL, { 0.0f, -kUpperArmLen * 0.5f, 0.0f },
      XMConvertToRadians(40.0f), XMConvertToRadians(20.0f) },
    { UpperArmR, ForearmR, { 0.0f, -kUpperArmLen * 0.5f, 0.0f },
      XMConvertToRadians(40.0f), XMConvertToRadians(20.0f) },
};

void Soldier::Pose(XMMATRIX out[SegmentCount], float walkPhase, float moveBlend)
{
    const float bob = kBobHeight * moveBlend * std::cos(walkPhase * 2.0f);

    out[Pelvis] = XMMatrixTranslation(0.0f, kPelvisY + bob, 0.0f);
    out[Torso] = XMMatrixTranslation(0.0f, kTorsoY + bob, kTorsoZ);
    out[Head] = XMMatrixTranslation(0.0f, kHeadY + bob, kHeadZ);

    for (int i = 0; i < 2; ++i)
    {
        const float side = (i == 0) ? -1.0f : 1.0f;
        // The two legs run half a cycle apart. Reading the stride off a phase
        // per leg rather than off one shared `swing` is what lets the knee ask
        // where in its own stride the leg is, which is the whole trick below.
        const float legPhase = walkPhase + (side < 0.0f ? XM_PI : 0.0f);
        const float stride = std::sin(legPhase) * kSwingAngle * moveBlend;
        // The foot is travelling forward over the half of the cycle where the
        // stride is growing, and that's the half the knee folds through, peaking
        // as the leg passes under the hip. Through the other half the foot is
        // planted and the leg is straight, so the bend clamps off at zero.
        const float bend = kKneeBend * moveBlend * std::max(0.0f, std::cos(legPhase));

        // The thigh pivots about its hip. Its frame's +Y runs up the bone, so
        // it sits half a thigh below the joint and rotates around it.
        out[ThighL + i] = XMMatrixTranslation(0.0f, -kThighLen * 0.5f, 0.0f) *
                          XMMatrixRotationX(-stride) *
                          XMMatrixTranslation(side * kHipX, kHipY, kHipZ);
        // And the shin hangs off the thigh the same way, folding backward about
        // the knee — half a shin below a joint that is itself half a thigh below
        // the thigh's own frame. The boot rides the bottom of the shin, so it
        // now lifts and swings instead of scuffing along under a stiff leg.
        out[ShinL + i] = XMMatrixTranslation(0.0f, -kShinLen * 0.5f, 0.0f) *
                         XMMatrixRotationX(bend) *
                         XMMatrixTranslation(0.0f, -kThighLen * 0.5f, 0.0f) * out[ThighL + i];

        // The arms don't stride — both hands stay on the weapon — so they only
        // ride the bob, and the elbow between them falls out of where the two
        // ends are.
        out[UpperArmL + i] = UpperArmFrame(side, bob);
        out[ForearmL + i] = ForearmFrame(side, bob);
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

    for (int i = 0; i < 2; ++i) // legs: suit cylinder down each bone, boot at the ankle
    {
        const Segment thigh = static_cast<Segment>(ThighL + i);
        const Segment shin = static_cast<Segment>(ShinL + i);
        part(thigh, Shape::CylinderLow, XMMatrixScaling(0.14f, kThighLen, 0.14f), suit);
        // The knee pad caps the joint from the thigh, the way the pauldrons cap
        // the shoulders. A sphere sitting on the pivot itself covers the gap the
        // bend opens up behind the leg from any angle it's seen at.
        part(thigh, Shape::SphereLow,
             XMMatrixScaling(0.15f, 0.15f, 0.15f) *
                 XMMatrixTranslation(0.0f, -kThighLen * 0.5f, 0.01f),
             plateDark);
        part(shin, Shape::CylinderLow, XMMatrixScaling(0.12f, kShinLen, 0.12f), suit);
        part(shin, Shape::Box,
             XMMatrixScaling(0.15f, 0.10f, 0.24f) *
                 XMMatrixTranslation(0.0f, -kShinLen * 0.5f - 0.02f, 0.05f),
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

    for (int i = 0; i < 2; ++i) // arms: pauldron to elbow to hand, gloved fist at the end
    {
        const Segment upper = static_cast<Segment>(UpperArmL + i);
        const Segment fore = static_cast<Segment>(ForearmL + i);
        part(upper, Shape::CylinderLow, XMMatrixScaling(0.11f, kUpperArmLen, 0.11f), suit);
        part(upper, Shape::SphereLow, // elbow pad, capping the joint from above
             XMMatrixScaling(0.13f, 0.13f, 0.13f) *
                 XMMatrixTranslation(0.0f, -kUpperArmLen * 0.5f, 0.0f),
             plateDark);
        part(fore, Shape::CylinderLow, XMMatrixScaling(0.10f, kForearmLen, 0.10f), suit);
        part(fore, Shape::SphereLow,
             XMMatrixScaling(0.11f, 0.11f, 0.11f) *
                 XMMatrixTranslation(0.0f, -kForearmLen * 0.5f, 0.0f),
             metal);
    }

    part(ForearmR, Shape::Box, // rifle body
         XMMatrixScaling(0.07f, 0.10f, 0.42f) * XMMatrixTranslation(0.11f, 0.77f, 0.26f) *
             RifleRebase(),
         metal);
    part(ForearmR, Shape::CylinderLow, // barrel, ending near the muzzle spawn point
         XMMatrixScaling(0.045f, 0.28f, 0.045f) * XMMatrixRotationX(XM_PIDIV2) *
             XMMatrixTranslation(0.11f, 0.79f, 0.58f) * RifleRebase(),
         metal);
}
