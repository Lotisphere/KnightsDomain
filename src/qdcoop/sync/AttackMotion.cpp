// QD Coop — procedural attack motions. See AttackMotion.h for the angle convention.
// No SDK includes: this file is also compiled into the SDK-free motion test executable.
#include "qdcoop/sync/AttackMotion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>

namespace qd::motion {
namespace {

// =====================================================================================================================
// Keyframe tables — THE place to tune. One row = one keyframe:  { t (0..1 of the attack), { {bone, pitch, yaw, roll} } }
// Bones not listed in a key are 0 at that key. Every motion starts and ends with an empty key (rest pose), and the
// segments between keys use cubic ease-in-out (smoothstep). Phases: windup -> strike (shortest segment) -> follow /
// hold -> recover.
//
// Cheat sheet (character frame, see AttackMotion.h):
//   UpperArm pitch +90  : arm points forward          pitch +180 : arm points straight up
//   right arm yaw  < 0  : swings across to the left   left arm yaw > 0 : swings across to the right
//   right arm roll < 0  : lifts out to the right side left arm roll > 0 : lifts out to the left side
//   LowerArm pitch > 0  : bends the elbow (forearm folds up / back)
//   Spine yaw > 0       : chest turns right (wind-up for a right-handed swing); the three spine bones ADD UP
//   Spine pitch < 0     : bends forward             > 0 : leans back
// =====================================================================================================================

struct Rot { Bone bone; float pitch, yaw, roll; };
constexpr int kMaxRots = 12;
struct Key { float t; Rot r[kMaxRots]; };
struct Motion { const char* name; float duration; const Key* keys; int count; };

template <size_t N>
constexpr Motion MakeMotion(const char* name, float duration, const Key (&keys)[N]) {
    return Motion{name, duration, keys, static_cast<int>(N)};
}

constexpr Bone S0 = Bone::Spine_000, S1 = Bone::Spine_001, S2 = Bone::Spine_002;
constexpr Bone UR = Bone::UpperArm_R, LR = Bone::LowerArm_R, HR = Bone::Hand_R;
constexpr Bone UL = Bone::UpperArm_L, LL = Bone::LowerArm_L, HL = Bone::Hand_L;

// ---------------------------------------------------------------- Sword (0.55 s)
constexpr Key kSwordDiagonal[] = {   // v0: top-right -> bottom-left diagonal cut
    {0.00f, {}},
    {0.35f, {{S0, 0, 10, 0}, {S1, 0, 10, 0}, {S2, 5, 8, 0}, {UR, 150, 0, -30}, {LR, 40, 0, 0}, {HR, 20, 0, 0}}},
    {0.55f, {{S0, 0, -10, 0}, {S1, -8, -10, 0}, {S2, 0, -8, 0}, {UR, 40, -50, 0}, {LR, 10, 0, 0}, {HR, -15, 0, 0}}},
    {0.68f, {{S0, 0, -12, 0}, {S1, -8, -12, 0}, {S2, 0, -8, 0}, {UR, 30, -60, 0}, {LR, 10, 0, 0}, {HR, -20, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kSwordSweep[] = {      // v1: left -> right horizontal sweep (backhand)
    {0.00f, {}},
    {0.35f, {{S0, 0, -10, 0}, {S1, 0, -10, 0}, {S2, 0, -8, 0}, {UR, 85, -70, 0}, {LR, 40, 0, 0}, {HR, 0, -20, 0}}},
    {0.55f, {{S0, 0, 10, 0}, {S1, 0, 10, 0}, {S2, 0, 8, 0}, {UR, 85, 50, 0}, {LR, 5, 0, 0}, {HR, 0, 10, 0}}},
    {0.68f, {{S0, 0, 12, 0}, {S1, 0, 12, 0}, {S2, 0, 10, 0}, {UR, 75, 70, 0}, {LR, 5, 0, 0}, {HR, 0, 15, 0}}},
    {1.00f, {}},
};
constexpr Key kSwordThrust[] = {     // v2: forward thrust
    {0.00f, {}},
    {0.35f, {{S0, 0, 8, 0}, {S1, 0, 8, 0}, {S2, 0, 6, 0}, {UR, 40, 0, -20}, {LR, 80, 0, 0}, {HR, -10, 0, 0}}},
    {0.55f, {{S0, 0, -10, 0}, {S1, -10, -10, 0}, {S2, 0, -6, 0}, {UR, 88, 0, 0}, {HR, -5, 0, 0}}},
    {0.70f, {{S0, 0, -8, 0}, {S1, -8, -8, 0}, {S2, 0, -5, 0}, {UR, 85, 0, 0}, {LR, 3, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Greatsword (0.9 s), two-handed
constexpr Key kGreatswordOverhead[] = {   // v0 (and v2): overhead two-handed chop
    {0.00f, {}},
    {0.40f, {{S1, 8, 0, 0}, {S2, 8, 0, 0}, {UR, 175, -10, -10}, {LR, 50, 0, 0}, {HR, 20, 0, 0},
             {UL, 175, 10, 10}, {LL, 50, 0, 0}, {HL, 20, 0, 0}}},
    {0.58f, {{S0, -12, 0, 0}, {S1, -12, 0, 0}, {S2, -10, 0, 0}, {UR, 45, -15, 0}, {LR, 5, 0, 0}, {HR, -10, 0, 0},
             {UL, 45, 15, 0}, {LL, 5, 0, 0}, {HL, -10, 0, 0}}},
    {0.72f, {{S0, -15, 0, 0}, {S1, -15, 0, 0}, {S2, -12, 0, 0}, {UR, 30, -15, 0}, {HR, -10, 0, 0},
             {UL, 30, 15, 0}, {HL, -10, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kGreatswordSweep[] = {      // v1: two-handed horizontal sweep, big waist twist (right -> left)
    {0.00f, {}},
    {0.40f, {{S0, 0, 18, 0}, {S1, 0, 15, 0}, {S2, 0, 12, 0}, {UR, 80, 45, 0}, {LR, 30, 0, 0},
             {UL, 80, 60, 0}, {LL, 40, 0, 0}}},
    {0.60f, {{S0, 0, -18, 0}, {S1, 0, -15, 0}, {S2, 0, -12, 0}, {UR, 80, -50, 0}, {LR, 5, 0, 0},
             {UL, 80, -35, 0}, {LL, 15, 0, 0}}},
    {0.74f, {{S0, 0, -22, 0}, {S1, -5, -18, 0}, {S2, 0, -14, 0}, {UR, 70, -70, 0}, {LR, 5, 0, 0},
             {UL, 70, -55, 0}, {LL, 15, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Dagger (0.35 s)
constexpr Key kDaggerStab[] = {         // v0: quick forward stab
    {0.00f, {}},
    {0.30f, {{S1, 0, 6, 0}, {UR, 35, 0, -15}, {LR, 70, 0, 0}}},
    {0.50f, {{S0, 0, -6, 0}, {S1, -6, -8, 0}, {UR, 85, 0, 0}}},
    {0.62f, {{S0, 0, -6, 0}, {S1, -6, -8, 0}, {UR, 82, 0, 0}, {LR, 5, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kDaggerBackhand[] = {     // v1: reverse-grip diagonal slash (up-left -> down-right)
    {0.00f, {}},
    {0.30f, {{S0, 0, -6, 0}, {S1, 0, -8, 0}, {UR, 120, -45, 0}, {LR, 45, 0, 0}, {HR, 20, 0, 0}}},
    {0.52f, {{S0, 0, 6, 0}, {S1, 0, 8, 0}, {UR, 45, 40, 0}, {LR, 5, 0, 0}, {HR, -20, 0, 0}}},
    {0.64f, {{S0, 0, 7, 0}, {S1, 0, 9, 0}, {UR, 35, 50, 0}, {LR, 5, 0, 0}, {HR, -20, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kDaggerDouble[] = {       // v2: two small stabs
    {0.00f, {}},
    {0.15f, {{S1, 0, 5, 0}, {UR, 35, 0, -15}, {LR, 70, 0, 0}}},
    {0.30f, {{S1, -5, -6, 0}, {UR, 85, 0, 0}}},
    {0.45f, {{S1, 0, 2, 0}, {UR, 50, 0, -10}, {LR, 50, 0, 0}}},
    {0.62f, {{S0, 0, -4, 0}, {S1, -6, -8, 0}, {UR, 88, 0, 0}}},
    {0.75f, {{S0, 0, -4, 0}, {S1, -4, -6, 0}, {UR, 80, 0, 0}, {LR, 5, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Spear (0.6 s)
constexpr Key kSpearThrust[] = {        // v0: two-handed level thrust, body leans in, right arm straight
    {0.00f, {}},
    {0.35f, {{S0, 0, 12, 0}, {S1, 4, 10, 0}, {UR, 50, 0, -25}, {LR, 70, 0, 0}, {UL, 60, 30, 0}, {LL, 40, 0, 0}}},
    {0.55f, {{S0, -8, -8, 0}, {S1, -10, -8, 0}, {S2, -6, 0, 0}, {UR, 88, -5, 0}, {UL, 75, 20, 0}, {LL, 10, 0, 0}}},
    {0.70f, {{S0, -8, -8, 0}, {S1, -10, -8, 0}, {S2, -6, 0, 0}, {UR, 86, -5, 0}, {LR, 3, 0, 0},
             {UL, 73, 20, 0}, {LL, 10, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kSpearHigh[] = {          // v1: high downward stab
    {0.00f, {}},
    {0.35f, {{S1, 6, 0, 0}, {S2, 6, 0, 0}, {UR, 165, 0, -15}, {LR, 60, 0, 0}, {UL, 120, 20, 0}, {LL, 40, 0, 0}}},
    {0.55f, {{S0, -10, 0, 0}, {S1, -12, 0, 0}, {S2, -8, 0, 0}, {UR, 70, 0, 0}, {HR, -30, 0, 0},
             {UL, 60, 20, 0}, {LL, 10, 0, 0}}},
    {0.70f, {{S0, -10, 0, 0}, {S1, -12, 0, 0}, {S2, -8, 0, 0}, {UR, 65, 0, 0}, {HR, -30, 0, 0},
             {UL, 55, 20, 0}, {LL, 10, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kSpearSweep[] = {         // v2: horizontal sweep with the shaft (right -> left)
    {0.00f, {}},
    {0.35f, {{S0, 0, 15, 0}, {S1, 0, 12, 0}, {S2, 0, 8, 0}, {UR, 75, 55, 0}, {LR, 20, 0, 0},
             {UL, 75, 70, 0}, {LL, 30, 0, 0}}},
    {0.55f, {{S0, 0, -15, 0}, {S1, 0, -12, 0}, {S2, 0, -8, 0}, {UR, 75, -50, 0}, {LR, 5, 0, 0},
             {UL, 75, -30, 0}, {LL, 10, 0, 0}}},
    {0.70f, {{S0, 0, -18, 0}, {S1, 0, -14, 0}, {S2, 0, -10, 0}, {UR, 70, -65, 0}, {LR, 5, 0, 0},
             {UL, 70, -45, 0}, {LL, 10, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Hammer / Club (0.85 s)
constexpr Key kHammerSlam[] = {         // v0 (and v2): overhead slam into the ground, spine bends forward
    {0.00f, {}},
    {0.42f, {{S0, 6, 0, 0}, {S1, 8, 0, 0}, {S2, 8, 0, 0}, {UR, 180, -10, -10}, {LR, 60, 0, 0}, {HR, 25, 0, 0},
             {UL, 180, 10, 10}, {LL, 60, 0, 0}, {HL, 25, 0, 0}}},
    {0.58f, {{S0, -18, 0, 0}, {S1, -18, 0, 0}, {S2, -12, 0, 0}, {UR, 25, -15, 0}, {HR, -15, 0, 0},
             {UL, 25, 15, 0}, {HL, -15, 0, 0}}},
    {0.75f, {{S0, -20, 0, 0}, {S1, -20, 0, 0}, {S2, -14, 0, 0}, {UR, 20, -15, 0}, {HR, -15, 0, 0},
             {UL, 20, 15, 0}, {HL, -15, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kHammerDiagonal[] = {     // v1: diagonal downward swing from the upper right
    {0.00f, {}},
    {0.42f, {{S0, 0, 15, 0}, {S1, 0, 12, 0}, {S2, 5, 8, 0}, {UR, 150, 0, -45}, {LR, 50, 0, 0},
             {UL, 130, 30, 10}, {LL, 50, 0, 0}}},
    {0.60f, {{S0, 0, -15, 0}, {S1, -12, -12, 0}, {S2, 0, -8, 0}, {UR, 35, -45, 0}, {LR, 5, 0, 0},
             {UL, 35, -25, 0}, {LL, 10, 0, 0}}},
    {0.75f, {{S0, 0, -17, 0}, {S1, -14, -14, 0}, {S2, 0, -10, 0}, {UR, 25, -55, 0}, {LR, 5, 0, 0},
             {UL, 25, -35, 0}, {LL, 10, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Axe (0.7 s)
constexpr Key kAxeDiagonal[] = {        // v0 (and v2): diagonal chop, bigger wind-up than the sword
    {0.00f, {}},
    {0.40f, {{S0, 0, 15, 0}, {S1, 0, 14, 0}, {S2, 8, 10, 0}, {UR, 170, 0, -45}, {LR, 70, 0, 0}, {HR, 30, 0, 0}}},
    {0.58f, {{S0, 0, -12, 0}, {S1, -14, -12, 0}, {S2, -6, -8, 0}, {UR, 35, -55, 0}, {LR, 5, 0, 0}, {HR, -20, 0, 0}}},
    {0.72f, {{S0, 0, -14, 0}, {S1, -15, -14, 0}, {S2, 0, -10, 0}, {UR, 25, -65, 0}, {LR, 5, 0, 0}, {HR, -20, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kAxeHorizontal[] = {      // v1: horizontal chop (right -> left)
    {0.00f, {}},
    {0.40f, {{S0, 0, 18, 0}, {S1, 0, 15, 0}, {S2, 0, 10, 0}, {UR, 80, 70, -10}, {LR, 50, 0, 0}, {HR, 0, 20, 0}}},
    {0.58f, {{S0, 0, -14, 0}, {S1, 0, -12, 0}, {S2, 0, -8, 0}, {UR, 85, -45, 0}, {LR, 5, 0, 0}, {HR, 0, -10, 0}}},
    {0.72f, {{S0, 0, -16, 0}, {S1, 0, -14, 0}, {S2, 0, -10, 0}, {UR, 80, -60, 0}, {LR, 5, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Scythe (0.8 s), two-handed
constexpr Key kScytheSweep[] = {        // v0 (and v2): wide arc, right -> left
    {0.00f, {}},
    {0.40f, {{S0, 0, 20, 0}, {S1, 4, 16, 0}, {S2, 0, 12, 0}, {UR, 90, 65, 0}, {LR, 30, 0, 0},
             {UL, 70, 75, 0}, {LL, 40, 0, 0}}},
    {0.62f, {{S0, 0, -20, 0}, {S1, -6, -16, 0}, {S2, 0, -12, 0}, {UR, 70, -60, 0}, {LR, 5, 0, 0},
             {UL, 65, -40, 0}, {LL, 10, 0, 0}}},
    {0.76f, {{S0, 0, -24, 0}, {S1, -6, -18, 0}, {S2, 0, -14, 0}, {UR, 60, -75, 0}, {LR, 5, 0, 0},
             {UL, 55, -55, 0}, {LL, 10, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kScytheReverse[] = {      // v1: reverse arc, left -> right
    {0.00f, {}},
    {0.40f, {{S0, 0, -20, 0}, {S1, 0, -16, 0}, {S2, 0, -12, 0}, {UR, 80, -60, 0}, {LR, 40, 0, 0},
             {UL, 75, -45, 0}, {LL, 30, 0, 0}}},
    {0.62f, {{S0, 0, 20, 0}, {S1, -6, 16, 0}, {S2, 0, 12, 0}, {UR, 75, 65, 0}, {LR, 5, 0, 0},
             {UL, 70, 75, 0}, {LL, 10, 0, 0}}},
    {0.76f, {{S0, 0, 24, 0}, {S1, -6, 18, 0}, {S2, 0, 14, 0}, {UR, 65, 80, 0}, {LR, 5, 0, 0},
             {UL, 60, 85, 0}, {LL, 10, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Fist (0.35 s); the idle arm keeps a guard up
constexpr Key kFistRight[] = {          // v0: right straight
    {0.00f, {}},
    {0.30f, {{S0, 0, 8, 0}, {S1, 0, 8, 0}, {UR, 40, 0, -20}, {LR, 90, 0, 0}, {UL, 45, 0, 0}, {LL, 90, 0, 0}}},
    {0.48f, {{S0, 0, -10, 0}, {S1, -5, -10, 0}, {S2, 0, -5, 0}, {UR, 88, -8, 0}, {UL, 45, 0, 0}, {LL, 90, 0, 0}}},
    {0.62f, {{S0, 0, -10, 0}, {S1, -5, -10, 0}, {S2, 0, -5, 0}, {UR, 85, -8, 0}, {LR, 5, 0, 0},
             {UL, 45, 0, 0}, {LL, 90, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kFistLeft[] = {           // v1: left straight (mirror of v0)
    {0.00f, {}},
    {0.30f, {{S0, 0, -8, 0}, {S1, 0, -8, 0}, {UL, 40, 0, 20}, {LL, 90, 0, 0}, {UR, 45, 0, 0}, {LR, 90, 0, 0}}},
    {0.48f, {{S0, 0, 10, 0}, {S1, -5, 10, 0}, {S2, 0, 5, 0}, {UL, 88, 8, 0}, {UR, 45, 0, 0}, {LR, 90, 0, 0}}},
    {0.62f, {{S0, 0, 10, 0}, {S1, -5, 10, 0}, {S2, 0, 5, 0}, {UL, 85, 8, 0}, {LL, 5, 0, 0},
             {UR, 45, 0, 0}, {LR, 90, 0, 0}}},
    {1.00f, {}},
};
constexpr Key kFistUppercut[] = {       // v2: right uppercut
    {0.00f, {}},
    {0.30f, {{S0, 0, 8, 0}, {S1, -10, 8, 0}, {S2, -5, 0, 0}, {UR, 15, 0, -10}, {LR, 70, 0, 0},
             {UL, 45, 0, 0}, {LL, 90, 0, 0}}},
    {0.50f, {{S0, 0, -10, 0}, {S1, 8, -10, 0}, {S2, 6, 0, 0}, {UR, 110, -10, 0}, {LR, 80, 0, 0}, {HR, -10, 0, 0},
             {UL, 45, 0, 0}, {LL, 90, 0, 0}}},
    {0.64f, {{S0, 0, -10, 0}, {S1, 8, -10, 0}, {S2, 6, 0, 0}, {UR, 105, -10, 0}, {LR, 80, 0, 0},
             {UL, 45, 0, 0}, {LL, 90, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Gun (0.4 s)
constexpr Key kGunShot[] = {            // raise both arms forward, quick 10 deg recoil kick, settle, lower
    {0.00f, {}},
    {0.35f, {{UR, 85, 0, 0}, {LR, 5, 0, 0}, {UL, 80, 25, 0}, {LL, 20, 0, 0}}},
    {0.45f, {{S2, 4, 0, 0}, {UR, 95, 0, 0}, {LR, 10, 0, 0}, {HR, 10, 0, 0}, {UL, 90, 25, 0}, {LL, 25, 0, 0}}},
    {0.60f, {{UR, 85, 0, 0}, {LR, 5, 0, 0}, {UL, 80, 25, 0}, {LL, 20, 0, 0}}},
    {0.75f, {{UR, 84, 0, 0}, {LR, 5, 0, 0}, {UL, 79, 25, 0}, {LL, 20, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- Shield (0.5 s)
constexpr Key kShieldPush[] = {         // left arm pushes forward to block, short hold
    {0.00f, {}},
    {0.30f, {{S0, 0, -5, 0}, {S1, 0, -5, 0}, {UL, 40, 0, 15}, {LL, 80, 0, 0}}},
    {0.50f, {{S0, 0, 8, 0}, {S1, -4, 8, 0}, {UL, 80, 25, 0}, {LL, 60, 0, 0}, {HL, 10, 0, 0}}},
    {0.78f, {{S0, 0, 8, 0}, {S1, -4, 8, 0}, {UL, 80, 25, 0}, {LL, 60, 0, 0}, {HL, 10, 0, 0}}},
    {1.00f, {}},
};

// ---------------------------------------------------------------- motion list (name, duration in seconds, keys)
constexpr Motion kMotions[] = {
    MakeMotion("sword.diagonal", 0.55f, kSwordDiagonal),           //  0
    MakeMotion("sword.sweep", 0.55f, kSwordSweep),                 //  1
    MakeMotion("sword.thrust", 0.55f, kSwordThrust),               //  2
    MakeMotion("greatsword.overhead", 0.90f, kGreatswordOverhead), //  3
    MakeMotion("greatsword.sweep", 0.90f, kGreatswordSweep),       //  4
    MakeMotion("dagger.stab", 0.35f, kDaggerStab),                 //  5
    MakeMotion("dagger.backhand", 0.35f, kDaggerBackhand),         //  6
    MakeMotion("dagger.double", 0.35f, kDaggerDouble),             //  7
    MakeMotion("spear.thrust", 0.60f, kSpearThrust),               //  8
    MakeMotion("spear.high", 0.60f, kSpearHigh),                   //  9
    MakeMotion("spear.sweep", 0.60f, kSpearSweep),                 // 10
    MakeMotion("hammer.slam", 0.85f, kHammerSlam),                 // 11
    MakeMotion("hammer.diagonal", 0.85f, kHammerDiagonal),         // 12
    MakeMotion("axe.diagonal", 0.70f, kAxeDiagonal),               // 13
    MakeMotion("axe.horizontal", 0.70f, kAxeHorizontal),           // 14
    MakeMotion("scythe.sweep", 0.80f, kScytheSweep),               // 15
    MakeMotion("scythe.reverse", 0.80f, kScytheReverse),           // 16
    MakeMotion("fist.right", 0.35f, kFistRight),                   // 17
    MakeMotion("fist.left", 0.35f, kFistLeft),                     // 18
    MakeMotion("fist.uppercut", 0.35f, kFistUppercut),             // 19
    MakeMotion("gun.shot", 0.40f, kGunShot),                       // 20
    MakeMotion("shield.push", 0.50f, kShieldPush),                 // 21
};

// Weapon class x variant -> index into kMotions ("same as 0" entries repeat variant 0).
constexpr int kVariantTable[][3] = {
    /* Sword      */ {0, 1, 2},
    /* Greatsword */ {3, 4, 3},
    /* Dagger     */ {5, 6, 7},
    /* Spear      */ {8, 9, 10},
    /* Hammer     */ {11, 12, 11},
    /* Axe        */ {13, 14, 13},
    /* Club       */ {11, 12, 11},
    /* Scythe     */ {15, 16, 15},
    /* Fist       */ {17, 18, 19},
    /* Gun        */ {20, 20, 20},
    /* Shield     */ {21, 21, 21},
    /* Tool       */ {0, 1, 2},
    /* Unknown    */ {0, 1, 2},
};
static_assert(std::size(kVariantTable) == static_cast<size_t>(WeaponClass::Unknown) + 1);

const Motion& Pick(WeaponClass wc, int variant) {
    size_t c = static_cast<size_t>(wc);
    if (c >= std::size(kVariantTable)) c = static_cast<size_t>(WeaponClass::Unknown);
    const int v = ((variant % 3) + 3) % 3;
    return kMotions[kVariantTable[c][v]];
}

constexpr const char* kBoneNames[kBoneCount] = {
    "",           "Pelvis",     "Spine_000",  "Spine_001",  "Spine_002", "Neck",       "Head",       "Clavicle_R",
    "UpperArm_R", "LowerArm_R", "Hand_R",     "Wrist_R",    "Clavicle_L", "UpperArm_L", "LowerArm_L", "Hand_L",
};

// ---------------------------------------------------------------- classification, first rule wins
struct Rule { WeaponClass wc; std::string_view keys[8]; };
constexpr Rule kRules[] = {
    {WeaponClass::Greatsword, {"GreatSword", "Zweihander", "Zerkhander", "WodasoGreatSword", "RustedGreatSword"}},
    {WeaponClass::Dagger,     {"Dagger", "Knife", "Katar", "Rapier", "PoisonPiercer", "Syringe", "Kabob"}},
    {WeaponClass::Spear,      {"Spear", "Lance", "Halberd", "Trident", "Harpo"}},
    {WeaponClass::Hammer,     {"Hammer", "MorningStar", "Mace", "Flail", "Bat", "CookingPot", "GolemArm"}},
    {WeaponClass::Club,       {"Club"}},
    {WeaponClass::Axe,        {"Axe"}},
    {WeaponClass::Scythe,     {"Scythe"}},
    {WeaponClass::Fist,       {"Cestus", "Claw"}},
    {WeaponClass::Gun,        {"Gun", "Rifle", "Shotgun", "Launcher", "Cross", "Slashgun", "SVT", "Jigeki"}},
    {WeaponClass::Shield,     {"Shield"}},
    {WeaponClass::Tool,       {"Torch", "Lamp", "SkullRod"}},
    {WeaponClass::Sword,      {"Sword", "Blade", "Voxord", "Wing"}},
};

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ContainsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && Lower(hay[i + k]) == Lower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

bool MatchSegment(std::string_view seg, WeaponClass& out) {
    for (const Rule& r : kRules) {
        for (std::string_view k : r.keys) {
            if (!k.empty() && ContainsNoCase(seg, k)) {
                out = r.wc;
                return true;
            }
        }
    }
    return false;
}

float Smooth(float u) { return u * u * (3.0f - 2.0f * u); }   // cubic ease-in-out

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

}  // namespace

// =====================================================================================================================

const char* WeaponClassName(WeaponClass wc) {
    switch (wc) {
        case WeaponClass::Sword: return "Sword";
        case WeaponClass::Greatsword: return "Greatsword";
        case WeaponClass::Dagger: return "Dagger";
        case WeaponClass::Spear: return "Spear";
        case WeaponClass::Hammer: return "Hammer";
        case WeaponClass::Axe: return "Axe";
        case WeaponClass::Club: return "Club";
        case WeaponClass::Scythe: return "Scythe";
        case WeaponClass::Fist: return "Fist";
        case WeaponClass::Gun: return "Gun";
        case WeaponClass::Shield: return "Shield";
        case WeaponClass::Tool: return "Tool";
        case WeaponClass::Unknown: return "Unknown";
    }
    return "Unknown";
}

WeaponClass ClassifyWeapon(std::string_view itemPath) {
    if (itemPath.empty()) return WeaponClass::Fist;   // default: bare hands punch
    // Deepest segment first: "/Game/Items/Weapons/Axe/FS_ItemData_Axe.FS_ItemData_Axe" -> "FS_ItemData_Axe", ...
    size_t end = itemPath.size();
    while (end > 0) {
        size_t begin = end;
        while (begin > 0) {
            const char c = itemPath[begin - 1];
            if (c == '/' || c == '.' || c == ':' || c == '\\') break;
            --begin;
        }
        WeaponClass wc{};
        if (begin < end && MatchSegment(itemPath.substr(begin, end - begin), wc)) return wc;
        if (begin == 0) break;
        end = begin - 1;
    }
    return WeaponClass::Sword;
}

const char* BoneName(Bone b) {
    const int i = static_cast<int>(b);
    return (i > 0 && i < kBoneCount) ? kBoneNames[i] : "";
}

Bone BoneFromName(std::string_view n) {
    for (int i = 1; i < kBoneCount; ++i)
        if (n == kBoneNames[i]) return static_cast<Bone>(i);
    return Bone::None;
}

float Duration(WeaponClass wc, int variant) { return Pick(wc, variant).duration; }

const char* MotionName(WeaponClass wc, int variant) { return Pick(wc, variant).name; }

void Sample(WeaponClass wc, int variant, float t, std::vector<BoneOffset>& out) {
    out.clear();
    const Motion& m = Pick(wc, variant);
    if (!(t > 0.0f) || t >= 1.0f || m.count < 2) return;   // rest pose at both ends (also rejects NaN)

    int i = 0;
    while (i + 2 < m.count && t > m.keys[i + 1].t) ++i;
    const Key& a = m.keys[i];
    const Key& b = m.keys[i + 1];
    const float span = b.t - a.t;
    const float u = span > 1e-6f ? std::clamp((t - a.t) / span, 0.0f, 1.0f) : 1.0f;
    const float s = Smooth(u);

    float acc[kBoneCount][3] = {};
    bool used[kBoneCount] = {};
    auto add = [&](const Key& k, float w) {
        for (const Rot& r : k.r) {
            const int bi = static_cast<int>(r.bone);
            if (bi <= 0 || bi >= kBoneCount) continue;
            acc[bi][0] += r.pitch * w;
            acc[bi][1] += r.yaw * w;
            acc[bi][2] += r.roll * w;
            used[bi] = true;
        }
    };
    add(a, 1.0f - s);
    add(b, s);
    for (int bi = 1; bi < kBoneCount; ++bi)   // enum order = parent -> child
        if (used[bi]) out.push_back(BoneOffset{kBoneNames[bi], acc[bi][0], acc[bi][1], acc[bi][2]});
}

void AddScaled(std::vector<BoneOffset>& out, const std::vector<BoneOffset>& src, float w) {
    for (const BoneOffset& s : src) {
        auto it = std::find_if(out.begin(), out.end(), [&](const BoneOffset& o) {
            return o.bone == s.bone || (o.bone && s.bone && std::strcmp(o.bone, s.bone) == 0);
        });
        if (it == out.end()) {
            out.push_back(BoneOffset{s.bone, s.pitch * w, s.yaw * w, s.roll * w});
        } else {
            it->pitch += s.pitch * w;
            it->yaw += s.yaw * w;
            it->roll += s.roll * w;
        }
    }
}

bool CheckTables(std::string& error) {
    for (const Motion& m : kMotions) {
        if (!(m.duration > 0.0f)) { error = std::format("{}: duration must be > 0", m.name); return false; }
        if (m.count < 2) { error = std::format("{}: needs at least 2 keys", m.name); return false; }
        if (m.keys[0].t != 0.0f || m.keys[m.count - 1].t != 1.0f) {
            error = std::format("{}: keys must start at t=0 and end at t=1", m.name);
            return false;
        }
        for (int k = 0; k < m.count; ++k) {
            const Key& key = m.keys[k];
            if (k > 0 && !(key.t > m.keys[k - 1].t)) {
                error = std::format("{}: key {} time {} is not after the previous key", m.name, k, key.t);
                return false;
            }
            for (const Rot& r : key.r) {
                if (r.bone == Bone::None) {
                    if (r.pitch != 0 || r.yaw != 0 || r.roll != 0) {
                        error = std::format("{}: key {} has angles without a bone", m.name, k);
                        return false;
                    }
                    continue;
                }
                if ((k == 0 || k == m.count - 1)) {
                    error = std::format("{}: first/last key must be empty (rest pose)", m.name);
                    return false;
                }
                if (r.bone == Bone::Pelvis || static_cast<int>(r.bone) >= kBoneCount) {
                    error = std::format("{}: key {} moves {} (lower body must stay still)", m.name, k, BoneName(r.bone));
                    return false;
                }
                if (std::fabs(r.pitch) > 200 || std::fabs(r.yaw) > 200 || std::fabs(r.roll) > 200) {
                    error = std::format("{}: key {} {} angle out of range", m.name, k, BoneName(r.bone));
                    return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- quaternions

Quat QuatFromRotator(float pitch, float yaw, float roll) {
    // Same formula as UE4 FRotator::Quaternion().
    const float hp = std::fmod(pitch, 360.0f) * kDegToRad * 0.5f;
    const float hy = std::fmod(yaw, 360.0f) * kDegToRad * 0.5f;
    const float hr = std::fmod(roll, 360.0f) * kDegToRad * 0.5f;
    const float SP = std::sin(hp), CP = std::cos(hp);
    const float SY = std::sin(hy), CY = std::cos(hy);
    const float SR = std::sin(hr), CR = std::cos(hr);
    Quat q;
    q.x = CR * SP * SY - SR * CP * CY;
    q.y = -CR * SP * CY - SR * CP * SY;
    q.z = CR * CP * SY - SR * SP * CY;
    q.w = CR * CP * CY + SR * SP * SY;
    return q;
}

Quat Mul(const Quat& a, const Quat& b) {
    Quat r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}

Quat Conjugate(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

Quat Normalize(const Quat& q) {
    const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!(n2 > 1e-12f)) return Quat{};
    const float inv = 1.0f / std::sqrt(n2);
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    // UE FQuat::RotateVector: T = 2 (Q x V); V' = V + W T + Q x T
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return Vec3{v.x + q.w * tx + (q.y * tz - q.z * ty),
                v.y + q.w * ty + (q.z * tx - q.x * tz),
                v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

Quat ToComponentSpace(const BoneOffset& o, float meshForwardYaw) {
    const Quat m = QuatFromRotator(0.0f, meshForwardYaw, 0.0f);
    const Quat r = QuatFromRotator(o.pitch, o.yaw, o.roll);
    return Normalize(Mul(Mul(m, r), Conjugate(m)));
}

}  // namespace qd::motion
