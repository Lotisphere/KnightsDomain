// QD Coop — procedural per-weapon-class attack motions for the remote-player puppet.
// Pure math: no SDK, no engine, no globals with side effects. Thread-safe (all tables are constexpr).
//
// Angle convention (all values in degrees, UE FRotator semantics):
//   Offsets are expressed in the CHARACTER frame: X = forward, Y = right, Z = up (UE left-handed convention).
//     pitch > 0 : rotates forward toward up      (a hanging arm swings forward, then up overhead)
//     yaw   > 0 : rotates forward toward right   (torso turns right = right shoulder goes back)
//     roll  > 0 : rotates right toward down      (right arm: roll < 0 lifts it out sideways; left arm: roll > 0)
//   PoseDriver maps the character frame into the mesh's component space with ToComponentSpace() (the knight mesh
//   faces +Y in its asset space, so the default mesh-forward yaw is +90). Each offset is a rotation of the bone about
//   its own position, layered parent -> child, so child bones inherit their parents' offsets (spine twists add up).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qd::motion {

enum class WeaponClass : uint8_t { Sword, Greatsword, Dagger, Spear, Hammer, Axe, Club, Scythe, Fist, Gun, Shield, Tool, Unknown };

const char* WeaponClassName(WeaponClass wc);

// By folder / asset name keywords, case-insensitive, first rule wins. The path is split into segments on
// '/', '.' and ':' and scanned from the deepest segment outward, so a generic outer folder (e.g. "Combat" contains
// "bat") cannot override the weapon's own folder. No keyword anywhere -> Sword. Empty path -> Unknown.
WeaponClass ClassifyWeapon(std::string_view itemPath);

struct BoneOffset { const char* bone; float pitch, yaw, roll; };   // character frame, degrees (see top of file)

// t in [0,1] = attack progress; variant (any int, taken modulo 3) selects the combo variation.
// Out is cleared and filled with the bones that move, in Bone enum order (PoseDriver re-sorts by the real skeleton's
// bone indices before applying). t <= 0 and t >= 1 give an empty list (= all-zero offsets, the rest pose).
void  Sample(WeaponClass wc, int variant, float t, std::vector<BoneOffset>& out);
float Duration(WeaponClass wc, int variant);   // seconds, > 0
const char* MotionName(WeaponClass wc, int variant);   // e.g. "sword.diagonal" (logs / debug UI)

// ---- skeleton (names confirmed on Knight_mesh_Skeleton). Enum order is parent -> child. ----
enum class Bone : uint8_t {
    None = 0,
    Pelvis, Spine_000, Spine_001, Spine_002, Neck, Head,
    Clavicle_R, UpperArm_R, LowerArm_R, Hand_R, Wrist_R,
    Clavicle_L, UpperArm_L, LowerArm_L, Hand_L,
    Count
};
constexpr int kBoneCount = static_cast<int>(Bone::Count);
const char* BoneName(Bone b);                 // "" for None/Count
Bone        BoneFromName(std::string_view n); // exact match, Bone::None if unknown

// out[i] += w * src[i] per bone (bones missing from out are appended). Used to fade out an interrupted attack.
void AddScaled(std::vector<BoneOffset>& out, const std::vector<BoneOffset>& src, float w);

// Sanity check of every keyframe table: times strictly increasing from 0 to 1, first/last key empty, no Pelvis
// (lower body must not move), |angle| <= 200. Returns false and describes the first problem in `error`.
bool CheckTables(std::string& error);

// ---- quaternion helpers (UE FQuat layout and FRotator::Quaternion() semantics) ----
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Vec3 { float x = 0, y = 0, z = 0; };
Quat QuatFromRotator(float pitch, float yaw, float roll);
Quat Mul(const Quat& a, const Quat& b);        // UE convention: Mul(a, b) applies b first, then a
Quat Conjugate(const Quat& q);
Quat Normalize(const Quat& q);
Vec3 Rotate(const Quat& q, const Vec3& v);
// Character-frame offset -> component-space rotation: M * R * M^-1 with M = yaw(meshForwardYaw).
Quat ToComponentSpace(const BoneOffset& o, float meshForwardYaw);

}  // namespace qd::motion
