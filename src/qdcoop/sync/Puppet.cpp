// QD Coop — remote player avatar. See Puppet.h.
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/FS_ItemData_classes.hpp"

#include "qdcoop/sync/Puppet.h"

#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qd::sync {
namespace {

constexpr const char* kTag = "puppet";

// Assets. First path that loads with the right class wins.
constexpr const char* kMeshPath     = "/Game/NPCs/Knight/Knight_mesh.Knight_mesh";
constexpr const char* kHelmetPath   = "/Game/NPCs/Knight/Helmet_00_SM.Helmet_00_SM";
constexpr const char* kSwordPath    = "/Game/Enemies/Boner_00/Equipment/ShinySword.ShinySword";
constexpr const char* kCylinderPath = "/Engine/BasicShapes/Cylinder.Cylinder";
constexpr const wchar_t* kHelmetSocket = L"Head";
constexpr const wchar_t* kSwordSocket  = L"Weapon_R";

constexpr float kWalkThreshold  = 30.0f;   // cm/s
// Locomotion: the remote speed is sampled at 20 Hz and jitters around the threshold, which used to flip
// idle <-> walk with a hard cut every few frames.
constexpr float kWalkStart      = 45.0f;   // cm/s (filtered): idle -> walk
constexpr float kWalkStop       = 18.0f;   // cm/s (filtered): walk -> idle
constexpr float kSpeedFilterHz  = 10.0f;   // low-pass on the reported speed
constexpr float kMinStateTime   = 0.15f;   // s: no idle <-> walk change sooner than this
constexpr float kAirDelay       = 0.12f;   // s in air before the in-air pose (steps and small drops stay in the walk)
constexpr float kBlendTime      = 0.20f;   // s: cross-fade between locomotion animations
constexpr float kRateFilterHz   = 6.0f;    // walk-cycle rate follows the speed smoothly (walk -> run)
constexpr float kWalkRefSpeed   = 150.0f;  // cm/s at play rate 1
constexpr float kAttackRate     = 1.4f;
constexpr float kDashRate       = 2.5f;

std::atomic<float> g_yawOffset{kDefaultMeshYawOffset};

struct Assets {
    bool tried = false;
    SDK::USkeletalMesh*     mesh = nullptr;
    SDK::UStaticMesh*       helmet = nullptr;
    SDK::UStaticMesh*       sword = nullptr;
    SDK::UStaticMesh*       cylinder = nullptr;
    SDK::UAnimationAsset*   idle = nullptr;
    SDK::UAnimationAsset*   walk = nullptr;
    SDK::UAnimationAsset*   attack = nullptr;
    SDK::UAnimationAsset*   death = nullptr;
    SDK::UAnimationAsset*   inAir = nullptr;
};
Assets& A() {
    static Assets a;
    return a;
}
std::atomic<bool> g_statTried{false}, g_statMesh{false};
std::atomic<int>  g_statAnims{0};

// Keep a LoadObject'ed asset alive for the rest of the session: nothing else may reference our cached animations, and a
// GC would leave dangling pointers. Equivalent of UObject::AddToRoot(): set EInternalObjectFlags::RootSet (1 << 30,
// UE 4.27) on the object's FUObjectItem (Object @+0x0, Flags @+0x8).
void Pin(SDK::UObject* obj) {
    if (!obj) return;
    SDK::TUObjectArray* objects = SDK::UObject::GObjects.GetTypedPtr();
    if (!objects) return;
    const int32_t idx = obj->Index;
    constexpr int32_t kPerChunk = SDK::TUObjectArray::ElementsPerChunk;
    if (idx < 0 || idx >= objects->NumElements || idx / kPerChunk >= objects->NumChunks) return;
    SDK::FUObjectItem* chunk = objects->GetDecrytedObjPtr()[idx / kPerChunk];
    if (!chunk) return;
    SDK::FUObjectItem* item = &chunk[idx % kPerChunk];
    if (item->Object != obj) return;
    auto* flags = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(item) + 0x8);
    std::atomic_ref<int32_t>(*flags).fetch_or(1 << 30);
}

SDK::UObject* LoadFirst(std::initializer_list<const char*> paths, SDK::UClass* cls, const char* what) {
    for (const char* p : paths) {
        SDK::UObject* o = ue::LoadObject(p);
        if (!o) continue;
        if (cls && !o->IsA(cls)) {
            QD_WARN(kTag, "{}: {} loaded but is a {}, skipping", what, p, ue::ClassName(o));
            continue;
        }
        Pin(o);
        QD_INFO(kTag, "{}: {}", what, p);
        return o;
    }
    QD_WARN(kTag, "{}: none of the candidate assets could be loaded", what);
    return nullptr;
}

// LoadAsset_Blocking can fail while the engine is mid level-load (our pump runs inside ProcessEvent),
// so missing animations are retried every 10 s, up to 12 times, instead of only once.
struct AnimSlots {
    SDK::UAnimationAsset *idle = nullptr, *walk = nullptr, *inAir = nullptr, *attack = nullptr, *death = nullptr;
    int retries = 0;
    uint64_t lastTryMs = 0;
};
AnimSlots& Loaded() { static AnimSlots s; return s; }

int LoadMissingAnims() {
    auto& l = Loaded();
    SDK::UClass* animCls = SDK::UAnimSequenceBase::StaticClass();
    auto load = [&](SDK::UAnimationAsset*& slot, std::initializer_list<const char*> paths, const char* what) {
        if (!slot) slot = static_cast<SDK::UAnimationAsset*>(LoadFirst(paths, animCls, what));
    };
    load(l.idle, {"/Game/NPCs/Knight/Knight_Idle_Standing.Knight_Idle_Standing"}, "anim idle");
    load(l.walk, {"/Game/Enemies/UndeadKnight/Animations/Knight_Undead_Walk.Knight_Undead_Walk"}, "anim walk");
    load(l.inAir, {"/Game/Enemies/UndeadKnight/Animations/Knight_Undead_InAir.Knight_Undead_InAir"}, "anim inair");
    // Plain AnimSequences are preferred; the montages are a fallback (single-node mode can play them).
    load(l.attack, {"/Game/Enemies/UndeadKnight/Animations/Knight_Undead_Attack.Knight_Undead_Attack",
                    "/Game/Enemies/UndeadKnight/Animations/Knight_Undead_Attack_Montage.Knight_Undead_Attack_Montage"}, "anim attack");
    load(l.death, {"/Game/Enemies/UndeadKnight/Animations/Knight_Undead_Death.Knight_Undead_Death",
                   "/Game/Enemies/UndeadKnight/Animations/Knight_Undead_Death_Montage.Knight_Undead_Death_Montage"}, "anim death");
    l.lastTryMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());

    auto& a = A();
    a.idle = l.idle ? l.idle : l.walk;
    a.walk = l.walk ? l.walk : a.idle;
    a.inAir = l.inAir ? l.inAir : a.idle;
    a.attack = l.attack;
    a.death = l.death;
    return (l.idle != nullptr) + (l.walk != nullptr) + (l.inAir != nullptr) + (l.attack != nullptr) + (l.death != nullptr);
}

void RetryMissingAnims() {
    auto& l = Loaded();
    if (g_statAnims >= 5 || l.retries >= 12 || static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) - l.lastTryMs < 10000) return;
    ++l.retries;
    const int before = g_statAnims;
    g_statAnims = LoadMissingAnims();
    if (g_statAnims != before) QD_INFO(kTag, "anim retry {}: {}/5 animations loaded", l.retries, static_cast<int>(g_statAnims));
}

void EnsureAssets() {
    auto& a = A();
    if (a.tried) { RetryMissingAnims(); return; }
    a.tried = true;

    a.mesh   = static_cast<SDK::USkeletalMesh*>(LoadFirst({kMeshPath}, SDK::USkeletalMesh::StaticClass(), "mesh"));
    a.helmet = nullptr;   // the knight mesh already has a mail coif; the extra helmet floated off the Head bone
    a.sword  = static_cast<SDK::UStaticMesh*>(LoadFirst({kSwordPath}, SDK::UStaticMesh::StaticClass(), "sword"));
    const int anims = LoadMissingAnims();

    if (!a.mesh) {
        a.cylinder = static_cast<SDK::UStaticMesh*>(LoadFirst({kCylinderPath}, SDK::UStaticMesh::StaticClass(), "fallback"));
        ui::Toast(ui::Tr("puppet.mesh_failed"), ui::ToastKind::Warning, 8.0f);
    } else if (anims < 5) {
        ui::Toast(ui::Tr("puppet.anims_failed"), ui::ToastKind::Warning, 6.0f);
    }
    g_statMesh = a.mesh != nullptr;
    g_statAnims = anims;
    g_statTried = true;
}

SDK::FName MakeFName(const wchar_t* s) {
    return SDK::UKismetStringLibrary::Conv_StringToName(UC::FString(s));
}

SDK::FTransform MakeTransform(const SDK::FVector& t = SDK::FVector(0, 0, 0), const SDK::FVector& s = SDK::FVector(1, 1, 1)) {
    SDK::FTransform x{};
    x.Rotation.X = x.Rotation.Y = x.Rotation.Z = 0;
    x.Rotation.W = 1;
    x.Translation = t;
    x.Scale3D = s;
    return x;
}

SDK::UStaticMeshComponent* AddStaticMesh(SDK::AActor* actor, SDK::UStaticMesh* mesh, bool manualAttach,
                                         const SDK::FTransform& rel) {
    if (!actor || !mesh) return nullptr;
    SDK::UActorComponent* c = actor->AddComponentByClass(SDK::UStaticMeshComponent::StaticClass(), manualAttach, rel, false);
    if (!c || !c->IsA(SDK::UStaticMeshComponent::StaticClass())) {
        QD_WARN(kTag, "AddComponentByClass(StaticMeshComponent) failed");
        return nullptr;
    }
    auto* smc = static_cast<SDK::UStaticMeshComponent*>(c);
    smc->SetMobility(SDK::EComponentMobility::Movable);
    smc->SetStaticMesh(mesh);
    smc->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
    smc->SetGenerateOverlapEvents(false);
    return smc;
}

void AttachToSocket(SDK::UStaticMeshComponent* comp, SDK::USkeletalMeshComponent* parent, const wchar_t* socket) {
    if (!comp || !parent) return;
    const bool ok = comp->K2_AttachToComponent(parent, MakeFName(socket), SDK::EAttachmentRule::SnapToTarget,
                                               SDK::EAttachmentRule::SnapToTarget, SDK::EAttachmentRule::KeepRelative, false);
    if (!ok) QD_WARN(kTag, "attach to socket failed");
}

float AnimLength(SDK::UAnimationAsset* anim) {
    if (anim && anim->IsA(SDK::UAnimSequenceBase::StaticClass())) {
        const float len = static_cast<SDK::UAnimSequenceBase*>(anim)->SequenceLength;
        if (len > 0.01f) return len;
    }
    return 1.0f;
}

// ------------------------------------------------------------------ held weapon

// Placement of the weapon visual relative to the Weapon_R socket. The first entry whose `match` occurs in the item path
// wins, so keep the "" catch-all LAST. Tune live in F5 → 调试 → 武器挂点, then paste the "复制为 C++" log line above it.
//   { item path substring, {{loc x, y, z cm}, {pitch, yaw, roll deg}, scale} }
struct WeaponFitEntry {
    const char* match;
    WeaponFit   fit;
};
const WeaponFitEntry kWeaponFits[] = {
    // In-game side-view tuning: these first-person visuals have their origin near the blade/head, so
    // the knight's hand ended up holding the wrong end. Shift along the visual's local -Y to put the grip in the hand.
    {"/HagDagger/", {{0.0f, -30.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f}},
    {"/Axe/SimpleAxe", {{0.0f, -40.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f}},
    {"", {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f}},
};

constexpr uint64_t kWeaponRetryMs = 10000;   // item / visual load failed (e.g. mid level-load): retry after this
constexpr int      kWeaponMaxTries = 3;      // per item path, then give up and keep the knight sword

uint64_t SteadyMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Shared with the menu (render thread).
std::mutex            g_fitMx;
std::string           g_localWeapon;           // guarded by g_fitMx
bool                  g_fitOverridden = false; // guarded by g_fitMx
std::string           g_fitOverridePath;       // guarded by g_fitMx
WeaponFit             g_fitOverride;           // guarded by g_fitMx
std::atomic<uint32_t> g_fitVersion{1};         // bumped on every override change; puppets re-apply when it moves

WeaponFit TableFit(std::string_view path) {
    for (const auto& e : kWeaponFits)
        if (e.match[0] == '\0' || path.find(e.match) != std::string_view::npos) return e.fit;
    return WeaponFit{};
}

WeaponFit FitFor(const std::string& path) {
    std::lock_guard lk(g_fitMx);
    if (g_fitOverridden && g_fitOverridePath == path) return g_fitOverride;
    return TableFit(path);
}

// "/Game/A/B/DA_Sword.DA_Sword" -> "DA_Sword"
std::string ShortItemName(std::string_view path) {
    const size_t cut = path.find_last_of("./");
    return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

bool IsClassObject(SDK::UObject* o) { return o && ue::IsValid(o) && o->IsA(SDK::UClass::StaticClass()); }

// Item path -> resolved (and pinned) visual class, or the failure count.
struct VisualEntry {
    SDK::UClass* cls = nullptr;
    int          tries = 0;
    uint64_t     lastTryMs = 0;
};
std::unordered_map<std::string, VisualEntry>& VisualCache() {
    static std::unordered_map<std::string, VisualEntry> m;
    return m;
}

// Game thread. FS_ItemData_C -> Default_Visual_BP (soft class: already-loaded first, else load its path) ->
// _Fallback_Visual_BP. nullptr when nothing usable; `permanent` = do not retry this item again.
SDK::UClass* ResolveVisualClass(const std::string& itemPath, bool& permanent) {
    permanent = false;
    VisualEntry& e = VisualCache()[itemPath];
    if (e.cls) {
        if (ue::IsValid(e.cls)) return e.cls;
        e.cls = nullptr;   // pinned, so this should never happen; load again
    }
    if (e.tries >= kWeaponMaxTries) {
        permanent = true;
        return nullptr;
    }
    const uint64_t now = SteadyMs();
    if (e.tries > 0 && now - e.lastTryMs < kWeaponRetryMs) return nullptr;
    ++e.tries;
    e.lastTryMs = now;

    SDK::UObject* obj = ue::LoadObject(itemPath);
    if (!obj) {
        QD_WARN(kTag, "weapon: item {} could not be loaded (try {}/{}); keeping the knight sword", itemPath, e.tries,
                kWeaponMaxTries);
        permanent = e.tries >= kWeaponMaxTries;
        return nullptr;
    }
    if (!obj->IsA(SDK::UFS_ItemData_C::StaticClass())) {
        QD_WARN(kTag, "weapon: {} is a {}, not FS_ItemData_C; keeping the knight sword", itemPath, ue::ClassName(obj));
        e.tries = kWeaponMaxTries;
        permanent = true;
        return nullptr;
    }
    auto* item = static_cast<SDK::UFS_ItemData_C*>(obj);
    Pin(item);

    SDK::UClass* cls = nullptr;
    std::string  source;
    if (SDK::UObject* o = item->Default_Visual_BP.Get(); IsClassObject(o)) {   // soft pointer already resolved
        cls = static_cast<SDK::UClass*>(o);
        source = "Default_Visual_BP";
    }
    if (!cls) {
        const auto& id = item->Default_Visual_BP.ObjectID;
        if (!id.AssetPathName.IsNone()) {
            std::string visualPath = id.AssetPathName.GetRawString();
            const std::string sub = id.SubPathString.ToString();
            if (!sub.empty()) visualPath += ":" + sub;
            SDK::UObject* o = ue::LoadObject(visualPath);
            if (IsClassObject(o)) {
                cls = static_cast<SDK::UClass*>(o);
                source = "Default_Visual_BP " + visualPath;
            } else {
                QD_WARN(kTag, "weapon: Default_Visual_BP {} of {} did not load as a class", visualPath, itemPath);
            }
        }
    }
    if (!cls && IsClassObject(item->_Fallback_Visual_BP)) {
        cls = item->_Fallback_Visual_BP;
        source = "_Fallback_Visual_BP";
    }
    if (!cls) {
        QD_WARN(kTag, "weapon: {} has no loadable visual class (try {}/{}); keeping the knight sword", itemPath, e.tries,
                kWeaponMaxTries);
        permanent = e.tries >= kWeaponMaxTries;
        return nullptr;
    }
    if (!cls->IsSubclassOf(SDK::AActor::StaticClass())) {
        QD_WARN(kTag, "weapon: visual {} of {} is not an Actor class; keeping the knight sword", ue::PathName(cls),
                itemPath);
        e.tries = kWeaponMaxTries;
        permanent = true;
        return nullptr;
    }
    Pin(cls);
    e.cls = cls;
    QD_INFO(kTag, "weapon: {} -> {} ({})", itemPath, ue::PathName(cls), source);
    return cls;
}

// A display-only copy of the weapon: no collision, no ticking (actor and every component), nothing beyond rendering.
// Shadow casting is left as authored. Everything is made Movable so the root can be attached to the moving avatar.
void QuietWeaponActor(SDK::AActor* a) {
    a->SetActorEnableCollision(false);
    a->SetActorTickEnabled(false);
    // K2_GetComponentsByClass returns an engine-allocated TArray the SDK container never frees: a few bytes leak per
    // weapon spawn (only on weapon changes), accepted.
    auto comps = a->K2_GetComponentsByClass(SDK::UActorComponent::StaticClass());
    for (int i = 0; i < comps.Num(); ++i) {
        SDK::UActorComponent* c = comps[i];
        if (!c || !ue::IsValid(c)) continue;
        c->SetComponentTickEnabled(false);
        if (c->IsA(SDK::USceneComponent::StaticClass()))
            static_cast<SDK::USceneComponent*>(c)->SetMobility(SDK::EComponentMobility::Movable);
        if (c->IsA(SDK::UPrimitiveComponent::StaticClass())) {
            auto* p = static_cast<SDK::UPrimitiveComponent*>(c);
            p->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
            p->SetGenerateOverlapEvents(false);
            p->SetSimulatePhysics(false);
            p->SetReceivesDecals(false);
            p->SetRenderCustomDepth(false);
        }
    }
}

}  // namespace

// ------------------------------------------------------------------ weapon fit API (thread-safe)

void SetLocalWeaponPath(const std::string& itemPath) {
    std::lock_guard lk(g_fitMx);
    g_localWeapon = itemPath;
}

WeaponTuneInfo DebugWeaponTuneInfo() {
    std::lock_guard lk(g_fitMx);
    WeaponTuneInfo info;
    info.path = g_localWeapon;
    info.overridden = g_fitOverridden && g_fitOverridePath == g_localWeapon;
    info.fit = info.overridden ? g_fitOverride : TableFit(g_localWeapon);
    return info;
}

void DebugSetWeaponFit(const WeaponFit& fit) {
    {
        std::lock_guard lk(g_fitMx);
        if (g_localWeapon.empty()) return;
        g_fitOverridden = true;
        g_fitOverridePath = g_localWeapon;
        g_fitOverride = fit;
    }
    g_fitVersion.fetch_add(1);
}

void DebugResetWeaponFit() {
    {
        std::lock_guard lk(g_fitMx);
        g_fitOverridden = false;
        g_fitOverridePath.clear();
    }
    g_fitVersion.fetch_add(1);
}

void DebugLogWeaponFitCpp() {
    const WeaponTuneInfo info = DebugWeaponTuneInfo();
    if (info.path.empty()) {
        QD_INFO(kTag, "weapon fit: no local weapon to print");
        return;
    }
    const WeaponFit& f = info.fit;
    QD_INFO(kTag, "weapon fit for {} -> paste into kWeaponFits (Puppet.cpp), above the \"\" entry:", info.path);
    QD_INFO(kTag, "    {{\"{}\", {{{{{:.1f}f, {:.1f}f, {:.1f}f}}, {{{:.1f}f, {:.1f}f, {:.1f}f}}, {:.2f}f}}}},",
            ShortItemName(info.path), f.loc[0], f.loc[1], f.loc[2], f.rot[0], f.rot[1], f.rot[2], f.scale);
}

float MeshYawOffset() { return g_yawOffset.load(std::memory_order_relaxed); }
void SetMeshYawOffset(float degrees) { g_yawOffset.store(degrees, std::memory_order_relaxed); }

PuppetAssetStatus GetPuppetAssetStatus() {
    PuppetAssetStatus s;
    s.tried = g_statTried;
    s.meshOk = g_statMesh;
    s.animsOk = g_statAnims;
    return s;
}

Puppet::Puppet(uint8_t slot) : slot_(slot) {}

Puppet::~Puppet() {
    Abandon();
}

bool Puppet::IsSpawned() const {
    if (!actor_ || !world_) return false;
    if (ue::World() != world_) return false;
    return ue::IsValid(actor_);
}

bool Puppet::Spawn(const net::Vec3f& feet, float yaw) {
    if (IsSpawned()) return true;
    Abandon();
    EnsureAssets();

    SDK::UWorld* world = ue::World();
    if (!world) return false;

    const SDK::FVector loc(feet.x, feet.y, feet.z);
    const SDK::FRotator rot(0.0f, yaw + MeshYawOffset(), 0.0f);
    SDK::AActor* spawned = ue::SpawnActor(SDK::ASkeletalMeshActor::StaticClass(), loc, rot);
    if (!spawned || !spawned->IsA(SDK::ASkeletalMeshActor::StaticClass())) {
        QD_WARN(kTag, "slot {}: SpawnActor(SkeletalMeshActor) failed", slot_);
        return false;
    }
    actor_ = static_cast<SDK::ASkeletalMeshActor*>(spawned);
    world_ = world;
    actor_->SetActorEnableCollision(false);

    auto& a = A();
    skel_ = actor_->SkeletalMeshComponent;
    animated_ = false;
    if (skel_) {
        skel_->SetMobility(SDK::EComponentMobility::Movable);
        skel_->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
        skel_->SetGenerateOverlapEvents(false);
    }
    if (skel_ && a.mesh) {
        // Original materials, no transparency/ghost effect. Shadows stay on.
        skel_->SetSkeletalMesh(a.mesh, true);
        skel_->SetAnimationMode(SDK::EAnimationMode::AnimationSingleNode);
        animated_ = true;
        AttachToSocket(AddStaticMesh(actor_, a.helmet, true, MakeTransform()), skel_, kHelmetSocket);
        // M1 knight sword: shown until the first MsgEquip arrives, and whenever the real weapon cannot be shown.
        sword_ = AddStaticMesh(actor_, a.sword, true, MakeTransform());
        AttachToSocket(sword_, skel_, kSwordSocket);
        // visible PoseableMesh on top of the (hidden, still animating) source; moves the sword onto it.
        pose_.SetMeshForwardYaw(-MeshYawOffset());
        if (!pose_.Attach(actor_, skel_)) QD_WARN(kTag, "slot {}: procedural attacks unavailable (PoseDriver::Attach failed)", slot_);
    } else {
        // Fallback: a scaled engine cylinder standing on the feet point (cylinder is 100 cm tall, centred on its origin).
        AddStaticMesh(actor_, a.cylinder, false, MakeTransform(SDK::FVector(0, 0, 90), SDK::FVector(0.6f, 0.6f, 1.8f)));
    }

    state_ = Anim::None;
    curAnim_ = nullptr;
    curLoop_ = false;
    curRate_ = 1.0f;
    attackLeft_ = 0;
    sinceAttack_ = 100.0f;
    lastAction_ = 0;
    feet_ = feet;
    yaw_ = yaw;
    QD_INFO(kTag, "slot {}: spawned at ({:.0f}, {:.0f}, {:.0f}){}", slot_, feet.x, feet.y, feet.z,
            animated_ ? "" : " [fallback]");
    if (weaponWanted_) {   // respawn (area change, world change): put the last known weapon back in the hand
        weaponRetryMs_ = 0;
        ApplyWeapon();
    }
    return true;
}

void Puppet::Destroy() {
    DestroyWeaponActor();   // attached actors are NOT destroyed with their parent: do it explicitly
    if (IsSpawned()) {
        actor_->K2_DestroyActor();
        QD_INFO(kTag, "slot {}: destroyed", slot_);
    }
    Abandon();
}

void Puppet::Abandon() {
    // If the old actors are still alive (e.g. we only *thought* the world changed), destroy them instead of leaking them.
    DestroyWeaponActor();
    if (actor_ && ue::IsValid(actor_)) {
        actor_->K2_DestroyActor();
        QD_INFO(kTag, "slot {}: destroyed stale actor", slot_);
    }
    pose_.Forget();
    actor_ = nullptr;
    skel_ = nullptr;
    sword_ = nullptr;
    world_ = nullptr;
    curAnim_ = nullptr;
    animated_ = false;
    state_ = Anim::None;
    // weaponPath_ / weaponWanted_ survive: the next Spawn() re-applies them.
}

SDK::AActor* Puppet::Actor() const {
    return IsSpawned() ? actor_ : nullptr;
}

// ---- held weapon

void Puppet::SetWeapon(const std::string& rawPath) {
    // The game models bare hands as /Game/Items/Weapons/NullWeapon (no visual): treat it as empty-handed (fists).
    const std::string itemPath = rawPath.find("NullWeapon") != std::string::npos ? std::string() : rawPath;
    if (weaponWanted_ && itemPath == weaponPath_) return;
    weaponWanted_ = true;
    weaponPath_ = itemPath;
    weaponDirty_ = true;
    weaponRetryMs_ = 0;
    if (IsSpawned()) ApplyWeapon();
}

void Puppet::DestroyWeaponActor() {
    if (weapon_ && ue::IsValid(weapon_)) weapon_->K2_DestroyActor();
    weapon_ = nullptr;
}

void Puppet::ShowSword(bool show) {
    if (sword_ && ue::IsValid(sword_)) sword_->SetVisibility(show, false);
}

void Puppet::ApplyWeapon() {
    weaponDirty_ = false;
    DestroyWeaponActor();
    if (!IsSpawned() || !animated_ || !skel_) return;   // the fallback cylinder has no hand to hold anything

    if (weaponPath_.empty()) {
        ShowSword(false);
        QD_INFO(kTag, "slot {}: empty-handed", slot_);
        return;
    }

    bool         permanent = false;
    SDK::UClass* cls = ResolveVisualClass(weaponPath_, permanent);
    if (!cls) {
        ShowSword(true);
        if (!permanent) {
            weaponDirty_ = true;
            weaponRetryMs_ = SteadyMs() + kWeaponRetryMs;
        }
        return;
    }

    SDK::AActor* a = ue::SpawnActor(cls, SDK::FVector(feet_.x, feet_.y, feet_.z), SDK::FRotator(0.0f, yaw_, 0.0f));
    if (!a || !ue::IsValid(a)) {
        QD_WARN(kTag, "slot {}: SpawnActor({}) failed; keeping the knight sword", slot_, ue::Name(cls));
        ShowSword(true);
        return;
    }
    weapon_ = a;
    QuietWeaponActor(a);

    SDK::USceneComponent* root = a->K2_GetRootComponent();
    SDK::USceneComponent* hand = pose_.VisibleMesh() ? pose_.VisibleMesh() : static_cast<SDK::USceneComponent*>(skel_);
    const bool attached = root && root->K2_AttachToComponent(hand, MakeFName(kSwordSocket), SDK::EAttachmentRule::SnapToTarget,
                                                             SDK::EAttachmentRule::SnapToTarget,
                                                             SDK::EAttachmentRule::KeepRelative, false);
    if (!attached) {
        QD_WARN(kTag, "slot {}: could not attach {} to Weapon_R; keeping the knight sword", slot_, ue::Name(cls));
        DestroyWeaponActor();
        ShowSword(true);
        return;
    }
    weaponBaseScale_[0] = root->RelativeScale3D.X;
    weaponBaseScale_[1] = root->RelativeScale3D.Y;
    weaponBaseScale_[2] = root->RelativeScale3D.Z;
    ApplyWeaponFit();
    ShowSword(false);
    QD_INFO(kTag, "slot {}: holding {} ({})", slot_, ShortItemName(weaponPath_), ue::Name(cls));
}

void Puppet::ApplyWeaponFit() {
    weaponFitVersion_ = g_fitVersion.load();
    if (!weapon_ || !ue::IsValid(weapon_)) return;
    SDK::USceneComponent* root = weapon_->K2_GetRootComponent();
    if (!root) return;
    const WeaponFit f = FitFor(weaponPath_);
    root->K2_SetRelativeLocationAndRotation(SDK::FVector(f.loc[0], f.loc[1], f.loc[2]),
                                            SDK::FRotator(f.rot[0], f.rot[1], f.rot[2]), false, nullptr, true);
    root->SetRelativeScale3D(SDK::FVector(weaponBaseScale_[0] * f.scale, weaponBaseScale_[1] * f.scale,
                                          weaponBaseScale_[2] * f.scale));
}

void Puppet::SetTransform(const net::Vec3f& feet, float yaw) {
    if (!IsSpawned()) return;
    const SDK::FVector loc(feet.x, feet.y, feet.z);
    const SDK::FRotator rot(0.0f, yaw + MeshYawOffset(), 0.0f);
    actor_->K2_SetActorLocationAndRotation(loc, rot, false, nullptr, true);
    feet_ = feet;
    yaw_ = yaw;
}

void Puppet::Play(SDK::UAnimationAsset* anim, bool loop, float rate, bool restart) {
    if (!skel_ || !anim) return;
    if (!restart && anim == curAnim_ && loop == curLoop_) {
        if (std::fabs(rate - curRate_) > 0.05f) {
            skel_->SetPlayRate(rate);
            curRate_ = rate;
        }
        return;
    }
    if (curAnim_ && pose_.IsAttached()) pose_.BlendBase(kBlendTime);   // must run before the switch (reads the old pose)
    skel_->PlayAnimation(anim, loop);
    skel_->SetPlayRate(rate);
    curAnim_ = anim;
    curLoop_ = loop;
    curRate_ = rate;
}

void Puppet::StartAttack() {
    auto& a = A();
    if (!animated_ || state_ == Anim::Dead) return;
    if (sinceAttack_ < 0.2f) return;   // the state edge and the PlayerEvent usually both fire for one swing
    if (pose_.IsAttached()) {
        // per-weapon procedural swing layered over locomotion (walk keeps playing underneath).
        combo_ = sinceAttack_ < 1.2f ? (combo_ + 1) % 3 : 0;
        sinceAttack_ = 0;
        pose_.PlayAttack(weaponWanted_ ? weaponPath_ : std::string("/Knight/Sword"), combo_);
        return;
    }
    if (!a.attack) return;
    sinceAttack_ = 0;
    state_ = Anim::Attack;
    attackLeft_ = AnimLength(a.attack) / kAttackRate;
    Play(a.attack, false, kAttackRate, true);
}

void Puppet::TriggerAttack() {
    if (IsSpawned()) StartAttack();
}

void Puppet::Update(uint16_t actionId, float speed2D, float dt) {
    if (!IsSpawned() || !animated_) return;
    EnsureAssets();   // cheap; retries animations that failed to load during level streaming
    pose_.SetMeshForwardYaw(-MeshYawOffset());
    pose_.Tick(dt);

    // Held weapon: retry a failed load, respawn a visual that vanished, follow the debug fit sliders.
    if (weaponWanted_) {
        const uint64_t nowMs = SteadyMs();
        if (weapon_ && !ue::IsValid(weapon_)) {
            weapon_ = nullptr;
            weaponDirty_ = true;
            weaponRetryMs_ = nowMs + 2000;
        }
        if (weaponDirty_ && nowMs >= weaponRetryMs_) ApplyWeapon();
        if (weapon_ && weaponFitVersion_ != g_fitVersion.load(std::memory_order_relaxed)) ApplyWeaponFit();
    }

    auto& a = A();
    sinceAttack_ += dt;
    if (attackLeft_ > 0) attackLeft_ -= dt;

    // 3: dead -> death anim once, hold the last frame (non-looping single node stops at the end)
    if (actionId == 3) {
        if (state_ != Anim::Dead) {
            state_ = Anim::Dead;
            attackLeft_ = 0;
            pose_.StopAttack();
            if (a.death) Play(a.death, false, 1.0f, true);
        }
        lastAction_ = actionId;
        return;
    }
    if (state_ == Anim::Dead) state_ = Anim::None;   // respawned

    if (actionId == 1 && lastAction_ != 1) StartAttack();
    lastAction_ = actionId;
    if (attackLeft_ > 0 && state_ == Anim::Attack) return;   // let the swing finish, then fall back to locomotion

    // ---- locomotion state machine
    if (!(dt > 0.0f) || !std::isfinite(dt)) dt = 0.0f;
    if (!std::isfinite(speed2D) || speed2D < 0) speed2D = 0;
    smoothSpeed_ += (speed2D - smoothSpeed_) * (1.0f - std::exp(-kSpeedFilterHz * dt));
    airTime_ = actionId == 4 ? airTime_ + dt : 0.0f;
    stateTime_ += dt;
    const Anim prev = state_;

    Anim want = prev;
    if (actionId == 4 && airTime_ >= kAirDelay) {
        want = Anim::InAir;
    } else if (actionId == 2) {
        want = Anim::Walk;   // dashing: the walk cycle, fast
    } else if (prev == Anim::Walk) {
        // stay in the walk until the filtered speed is clearly low, and never flip back within kMinStateTime
        if (smoothSpeed_ < kWalkStop && stateTime_ >= kMinStateTime) want = Anim::Idle;
    } else if (prev == Anim::Idle) {
        if (smoothSpeed_ > kWalkStart && stateTime_ >= kMinStateTime) want = Anim::Walk;
    } else {
        // from in-air / attack / spawn: pick by speed right away (landing while running keeps running)
        want = std::max(smoothSpeed_, speed2D) > kWalkThreshold ? Anim::Walk : Anim::Idle;
    }
    if (want != prev) stateTime_ = 0;
    state_ = want;

    switch (want) {
        case Anim::InAir:
            Play(a.inAir, true, 1.0f);
            return;
        case Anim::Walk: {
            // one cycle for walking and running: the rate follows the speed, filtered so walk -> run ramps up
            const float target = actionId == 2 ? kDashRate : std::clamp(smoothSpeed_ / kWalkRefSpeed, 0.7f, 2.5f);
            if (prev != Anim::Walk) walkRate_ = target;
            else walkRate_ += (target - walkRate_) * (1.0f - std::exp(-kRateFilterHz * dt));
            Play(a.walk, true, walkRate_);
            return;
        }
        default:
            Play(a.idle, true, 1.0f);
            return;
    }
}

}  // namespace qd::sync
