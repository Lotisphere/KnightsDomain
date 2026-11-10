// QD Coop — remote player avatar ("puppet"). Engine ASkeletalMeshActor + Knight NPC mesh, single-node
// animation state machine; plus the remote player's real weapon (the item's Default_Visual_BP actor on Weapon_R).
// Game thread only. The destructor goes through Abandon(), which destroys the avatar / weapon actors only if they are
// still valid UObjects.
#pragma once
#include "qdcoop/net/Protocol.h"
#include "qdcoop/sync/PoseDriver.h"

#include <cstdint>
#include <string>

namespace SDK {
class UWorld; class AActor; class ASkeletalMeshActor; class USkeletalMeshComponent; class UStaticMeshComponent;
class UAnimationAsset;
}

namespace qd::sync {

// Knight_mesh faces +Y in its asset space (UE convention for imported skeletal meshes) -> rotate -90 deg. UNVERIFIED in game:
// the F5 debug tab exposes a live slider (SetMeshYawOffset) so this can be tuned without rebuilding.
constexpr float kDefaultMeshYawOffset = -90.0f;
float MeshYawOffset();                  // thread-safe
void  SetMeshYawOffset(float degrees);  // thread-safe

struct PuppetAssetStatus {
    bool tried = false;       // load attempted
    bool meshOk = false;      // Knight mesh loaded (false => fallback cylinder)
    int  animsOk = 0;         // how many of the 5 animations resolved
};
PuppetAssetStatus GetPuppetAssetStatus();   // thread-safe snapshot

// ---- held weapon ------------------------------------------------------------------------------------------
// Placement of the weapon's visual actor relative to the Weapon_R socket. rot = pitch, yaw, roll (degrees); scale
// multiplies the visual actor's own root scale. Defaults come from kWeaponFits in Puppet.cpp (first entry whose
// substring occurs in the item path; the "" entry is the catch-all).
struct WeaponFit {
    float loc[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
    float scale = 1.0f;
};

// Debug tuning (F5 → 调试 → 武器挂点). The weapon being tuned is the LOCAL player's current one: the debug dummy carries
// it, so this works with a single instance; the live override also applies to every remote puppet holding that item.
struct WeaponTuneInfo {
    std::string path;              // item data asset path; empty = nothing read yet / empty hands
    WeaponFit   fit;               // effective fit (override if set, else the table)
    bool        overridden = false;
};
void           SetLocalWeaponPath(const std::string& itemPath);   // PlayerSync (game thread); thread-safe
WeaponTuneInfo DebugWeaponTuneInfo();                              // thread-safe
void           DebugSetWeaponFit(const WeaponFit& fit);            // thread-safe; applies live
void           DebugResetWeaponFit();                              // thread-safe; back to the table value
void           DebugLogWeaponFitCpp();                             // thread-safe; logs a kWeaponFits line

class Puppet {
public:
    explicit Puppet(uint8_t slot);
    ~Puppet();                              // forgets the actor, never calls into the engine
    Puppet(const Puppet&) = delete;
    Puppet& operator=(const Puppet&) = delete;

    bool Spawn(const net::Vec3f& feet, float yaw);   // loads + caches assets on first use (may hitch once)
    void Destroy();                                  // K2_DestroyActor if the actor is still alive in the current world
    void Abandon();                                  // world changed: drop pointers without touching them
    bool IsSpawned() const;

    void SetTransform(const net::Vec3f& feet, float yaw);
    // actionId: animation action; speed2D in cm/s. Drives the animation state machine.
    void Update(uint16_t actionId, float speed2D, float dt);
    void TriggerAttack();                            // PlayerEvent::Attack (also detected from actionId edges)

    // show this item's visual actor in the right hand instead of the knight sword. "" = empty-handed. Until the
    // first call (no MsgEquip received yet) the knight sword stays. Remembered across Spawn/Destroy; if the item cannot
    // be loaded the knight sword is kept (WARN in the log). Cheap when the path did not change.
    void SetWeapon(const std::string& itemPath);
    SDK::AActor* Actor() const;                      // the avatar actor, nullptr unless spawned

    const net::Vec3f& Feet() const { return feet_; }
    uint8_t Slot() const { return slot_; }

private:
    enum class Anim : uint8_t { None, Idle, Walk, Attack, Dead, InAir };

    void Play(SDK::UAnimationAsset* anim, bool loop, float rate, bool restart = false);
    void StartAttack();

    void ApplyWeapon();          // (re)spawns the weapon actor for weaponPath_ on the spawned avatar
    void ApplyWeaponFit();       // offset/rotation/scale from the table or the debug override
    void DestroyWeaponActor();   // destroys the weapon actor if it is still alive; forgets it
    void ShowSword(bool show);

    uint8_t                       slot_ = 0;
    SDK::UWorld*                  world_ = nullptr;
    SDK::ASkeletalMeshActor*      actor_ = nullptr;
    SDK::USkeletalMeshComponent*  skel_ = nullptr;
    bool                          animated_ = false;   // false for the fallback cylinder

    SDK::UStaticMeshComponent*    sword_ = nullptr;    // the M1 knight sword (fallback when no weapon visual)
    SDK::AActor*                  weapon_ = nullptr;   // spawned weapon visual actor, attached to Weapon_R
    std::string                   weaponPath_;         // last SetWeapon() value
    bool                          weaponWanted_ = false;   // SetWeapon() was called at least once
    bool                          weaponDirty_ = false;    // ApplyWeapon() still has to run (or retry)
    uint64_t                      weaponRetryMs_ = 0;      // steady ms; no ApplyWeapon() retry before this
    uint32_t                      weaponFitVersion_ = 0;   // fit version last applied (debug sliders bump it)
    float                         weaponBaseScale_[3] = {1, 1, 1};   // the visual actor's own root scale

    net::Vec3f                    feet_{};
    float                         yaw_ = 0;

    Anim                          state_ = Anim::None;
    SDK::UAnimationAsset*         curAnim_ = nullptr;
    bool                          curLoop_ = false;
    float                         curRate_ = 1.0f;
    float                         attackLeft_ = 0;
    float                         sinceAttack_ = 100.0f;
    uint16_t                      lastAction_ = 0;
    // Locomotion state machine: filtered speed, hysteresis, minimum time in a state, cross-fades.
    float                         smoothSpeed_ = 0;
    float                         stateTime_ = 0;       // seconds in state_
    float                         airTime_ = 0;         // seconds the remote player has reported "in air"
    float                         walkRate_ = 1.0f;     // filtered play rate of the walk cycle

    // Procedural attacks.
    motion::PoseDriver            pose_;
    int                           combo_ = 0;
};

}  // namespace qd::sync
