// QD Coop — drives procedural attack motions on a puppet through a UPoseableMeshComponent.
// Game thread only. Never touches the engine from its destructor (safe at DLL unload, same rule as Puppet).
//
//   actor
//    +- source  USkeletalMeshComponent  (single-node anims keep playing; hidden, but forced to keep ticking its pose)
//    +- poseable UPoseableMeshComponent (same mesh, visible, attached to source; weapons go on ITS Weapon_R socket)
//   every Tick: poseable.CopyPoseFromSkeletalComponent(source) -> layer the AttackMotion bone offsets on top.
#pragma once
#include "qdcoop/sync/AttackMotion.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SDK {
class AActor; class USceneComponent; class USkeletalMeshComponent; class UPoseableMeshComponent;
}

namespace qd::motion {

// How the source mesh is hidden while it keeps animating. Process-wide, applies to
// the next Attach(). HiddenInGame is the default; CollapseBones is the fallback if a hidden source stops updating.
enum class SourceHideMode : uint8_t {
    HiddenInGame,    // SetHiddenInGame(true) + VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones
    CollapseBones,   // stays "visible" (so bRecentlyRendered stays true) but HideBoneByName(root) collapses every vertex
};
void           SetSourceHideMode(SourceHideMode mode);

// Live tuning (dev commands "handfix"/"amp"): extra Hand_R rotation (character frame, degrees) blended in with a
// sin(pi*t) envelope during attacks of that class, and an amplitude multiplier for the whole motion. Thread-safe.
struct ClassTune { float hand[3][3] = {}; float amp = 1.0f; };   // hand[variant][pitch,yaw,roll]
void      SetClassTune(WeaponClass wc, const ClassTune& t);
ClassTune GetClassTune(WeaponClass wc);
// Dev: freeze attacks at progress t in [0,1] (t < 0 = off); force the combo variant (-1 = off). Thread-safe.
void DebugFreeze(float t);
void DebugForceVariant(int variant);
SourceHideMode GetSourceHideMode();

class PoseDriver {
public:
    PoseDriver() = default;
    ~PoseDriver() = default;               // forgets pointers only, never calls into the engine
    PoseDriver(const PoseDriver&) = delete;
    PoseDriver& operator=(const PoseDriver&) = delete;

    // Adds a PoseableMeshComponent (AddComponentByClass) with the same mesh as `source`, attached to `source` with an
    // identity transform, and hides `source` while keeping its pose updating. On failure returns false and leaves the
    // actor as it was (only `source` visible).
    bool Attach(SDK::AActor* actor, SDK::USkeletalMeshComponent* source);
    // Moves anything attached to the poseable back onto the source (same socket, relative offsets kept), destroys the
    // poseable and makes the source visible again. Safe to call when not attached.
    void Detach();
    // The actor died / the world changed: drop all pointers without touching the engine (Puppet::Abandon path).
    void Forget();

    bool IsAttached() const { return poseable_ != nullptr; }
    SDK::USceneComponent* VisibleMesh() const;   // weapons attach here: the poseable, or the source when not attached

    void PlayAttack(const std::string& itemPath, int variant);   // starts (or restarts, with a short blend) an attack
    void StopAttack();                                           // fades the current attack out (death, teleport, ...)
    // Cross-fade of the base (locomotion) pose: call right BEFORE switching the source's animation. Remembers the
    // source's current pose and blends from it to the new animation over `seconds` (idle <-> walk without a pop).
    void BlendBase(float seconds);
    void Tick(float dt);                                         // copy pose + layer the current attack offsets
    bool IsAttacking() const { return attacking_; }

    // Character-forward direction in the mesh's component space, as a yaw in degrees. The knight mesh faces +Y, so 90.
    // Puppet should pass -MeshYawOffset() so the F5 debug slider keeps both in sync.
    void  SetMeshForwardYaw(float degrees) { meshForwardYaw_ = degrees; }
    float MeshForwardYaw() const { return meshForwardYaw_; }

    WeaponClass CurrentClass() const { return class_; }
    float       AttackDuration() const { return duration_; }   // seconds, of the current / last attack

private:
    void ResolveBones();
    void ApplyOffsets(const std::vector<BoneOffset>& offsets);
    void HideSource();
    void ShowSource();
    void BeginFade();

    SDK::AActor*                   actor_ = nullptr;
    SDK::USkeletalMeshComponent*   source_ = nullptr;
    SDK::UPoseableMeshComponent*   poseable_ = nullptr;
    const void*                    mesh_ = nullptr;          // USkeletalMesh* the bone indices were resolved for
    SourceHideMode                 hiddenWith_ = SourceHideMode::HiddenInGame;

    int16_t                        boneIndex_[kBoneCount] = {};   // -1 = not in this skeleton
    Bone                           order_[kBoneCount] = {};       // bones sorted by real skeleton index (parent first)
    int                            orderCount_ = 0;

    float                          meshForwardYaw_ = 90.0f;

    std::string                    lastPath_;
    bool                           classified_ = false;
    WeaponClass                    class_ = WeaponClass::Unknown;
    int                            variant_ = 0;
    bool                           attacking_ = false;
    float                          elapsed_ = 0.0f;
    float                          duration_ = 0.0f;

    std::vector<BoneOffset>        current_;   // offsets applied this frame (attack + fade)
    std::vector<BoneOffset>        fadeFrom_;  // snapshot of an interrupted attack, faded out over kFadeTime
    float                          fadeLeft_ = 0.0f;

    struct Xf { float r[4]; float t[3]; float s[3]; };   // component-space bone transform
    std::vector<std::string>       allBoneNames_;   // every bone of the mesh, skeleton order (parents first)
    std::vector<Xf>                blendFrom_;      // source pose when BlendBase() was called
    std::vector<Xf>                blendLive_;      // scratch
    float                          blendLeft_ = 0.0f;
    float                          blendTime_ = 0.0f;
};

}  // namespace qd::motion
