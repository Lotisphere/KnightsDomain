// QD Coop — procedural attack pose driver. See PoseDriver.h.
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"

#include "qdcoop/sync/PoseDriver.h"

#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace qd::motion {
extern std::atomic<float> g_freezeT;      // dev freeze, defined below
extern std::atomic<int>   g_forceVariant;
namespace {

constexpr const char* kTag = "posedriver";
constexpr float kFadeTime = 0.12f;   // s, blend-out of an interrupted / stopped attack

std::atomic<uint8_t> g_hideMode{static_cast<uint8_t>(SourceHideMode::HiddenInGame)};

// Bone FNames are process-wide (FName = name-table index), so one cache serves every puppet. Game thread only.
SDK::FName g_boneNames[kBoneCount];
bool       g_namesReady = false;

void EnsureBoneNames() {
    if (g_namesReady) return;
    for (int i = 1; i < kBoneCount; ++i) g_boneNames[i] = ue::MakeName(BoneName(static_cast<Bone>(i)));
    g_namesReady = true;
}

SDK::FTransform IdentityTransform() {
    SDK::FTransform x{};
    x.Rotation.X = x.Rotation.Y = x.Rotation.Z = 0;
    x.Rotation.W = 1;
    x.Translation = SDK::FVector(0, 0, 0);
    x.Scale3D = SDK::FVector(1, 1, 1);
    return x;
}

// Re-parents every attach child of `from` (except `skip`) onto `to`, same socket name, relative transform kept.
// Iterates backwards: re-attaching removes the child from from->AttachChildren, which only shifts later indices.
int MoveChildren(SDK::USceneComponent* from, SDK::USceneComponent* to, const SDK::USceneComponent* skip) {
    int moved = 0;
    for (int32_t i = from->GetNumChildrenComponents() - 1; i >= 0; --i) {
        SDK::USceneComponent* child = from->GetChildComponent(i);
        if (!child || child == skip || child == to || !ue::IsValid(child)) continue;
        const SDK::FName socket = child->GetAttachSocketName();
        if (child->K2_AttachToComponent(to, socket, SDK::EAttachmentRule::KeepRelative, SDK::EAttachmentRule::KeepRelative,
                                        SDK::EAttachmentRule::KeepRelative, false)) {
            ++moved;
        } else {
            QD_WARN(kTag, "could not move attachment {} ({})", ue::Name(child), ue::ToString(socket));
        }
    }
    return moved;
}

}  // namespace

void SetSourceHideMode(SourceHideMode mode) { g_hideMode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed); }
SourceHideMode GetSourceHideMode() { return static_cast<SourceHideMode>(g_hideMode.load(std::memory_order_relaxed)); }

// ---------------------------------------------------------------------------------------------------------- attach

bool PoseDriver::Attach(SDK::AActor* actor, SDK::USkeletalMeshComponent* source) {
    if (poseable_) Detach();
    if (!ue::IsValid(actor) || !ue::IsValid(source)) return false;
    SDK::USkeletalMesh* mesh = source->SkeletalMesh;
    if (!mesh) {
        QD_WARN(kTag, "attach: source has no skeletal mesh");
        return false;
    }
    EnsureBoneNames();

    SDK::UActorComponent* c =
        actor->AddComponentByClass(SDK::UPoseableMeshComponent::StaticClass(), true, IdentityTransform(), false);
    if (!c || !c->IsA(SDK::UPoseableMeshComponent::StaticClass())) {
        QD_WARN(kTag, "attach: AddComponentByClass(PoseableMeshComponent) failed");
        return false;
    }
    auto* p = static_cast<SDK::UPoseableMeshComponent*>(c);
    p->SetMobility(SDK::EComponentMobility::Movable);
    p->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
    p->SetGenerateOverlapEvents(false);
    p->SetSkeletalMesh(mesh, true);   // registered component -> AllocateTransformData -> RequiredBones valid
    p->VisibilityBasedAnimTickOption = SDK::EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    const int32_t numMaterials = source->GetNumMaterials();   // mirror any material overrides on the source
    for (int32_t i = 0; i < numMaterials; ++i)
        if (SDK::UMaterialInterface* m = source->GetMaterial(i)) p->SetMaterial(i, m);

    if (!p->K2_AttachToComponent(source, SDK::FName(), SDK::EAttachmentRule::SnapToTarget,
                                 SDK::EAttachmentRule::SnapToTarget, SDK::EAttachmentRule::SnapToTarget, false)) {
        QD_WARN(kTag, "attach: could not attach the poseable to the source mesh");
        actor->K2_DestroyComponent(p);
        return false;
    }

    actor_ = actor;
    source_ = source;
    poseable_ = p;
    mesh_ = mesh;
    ResolveBones();
    if (orderCount_ == 0) {
        QD_WARN(kTag, "attach: none of the expected bones exist on {}, giving up", ue::Name(mesh));
        Detach();
        return false;
    }

    const int moved = MoveChildren(source_, poseable_, poseable_);   // helmet / weapon now follow the attack pose
    p->CopyPoseFromSkeletalComponent(source_);
    HideSource();
    QD_INFO(kTag, "attached to {}: {}/{} bones, {} attachment(s) moved, hide mode {}", ue::Name(actor), orderCount_,
            kBoneCount - 1, moved, static_cast<int>(hiddenWith_));
    return true;
}

void PoseDriver::Detach() {
    const bool sourceOk = source_ && ue::IsValid(source_);
    if (poseable_ && ue::IsValid(poseable_)) {
        if (sourceOk) MoveChildren(poseable_, source_, nullptr);
        if (actor_ && ue::IsValid(actor_)) actor_->K2_DestroyComponent(poseable_);
    }
    if (sourceOk) ShowSource();
    Forget();
}

void PoseDriver::Forget() {
    actor_ = nullptr;
    source_ = nullptr;
    poseable_ = nullptr;
    mesh_ = nullptr;
    orderCount_ = 0;
    attacking_ = false;
    elapsed_ = 0.0f;
    fadeLeft_ = 0.0f;
    current_.clear();
    fadeFrom_.clear();
}

SDK::USceneComponent* PoseDriver::VisibleMesh() const {
    if (poseable_) return poseable_;
    return source_;
}

void PoseDriver::BlendBase(float seconds) {
    if (!poseable_ || !source_ || allBoneNames_.empty() || !(seconds > 0.0f)) return;
    if (!ue::IsValid(poseable_) || !ue::IsValid(source_)) return;
    blendFrom_.resize(allBoneNames_.size());
    for (size_t i = 0; i < allBoneNames_.size(); ++i) {
        const SDK::FTransform t = source_->GetSocketTransform(ue::MakeName(allBoneNames_[i]),
                                                              SDK::ERelativeTransformSpace::RTS_Component);
        blendFrom_[i] = Xf{{t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W},
                           {t.Translation.X, t.Translation.Y, t.Translation.Z},
                           {t.Scale3D.X, t.Scale3D.Y, t.Scale3D.Z}};
    }
    blendLeft_ = blendTime_ = seconds;
}

void PoseDriver::ResolveBones() {
    allBoneNames_.clear();
    blendLeft_ = 0.0f;
    {
        const int32_t n = poseable_->GetNumBones();
        for (int32_t i = 0; i < n && i < 512; ++i) allBoneNames_.push_back(ue::ToString(poseable_->GetBoneName(i)));
    }
    struct Entry { int32_t index; Bone bone; };
    Entry found[kBoneCount] = {};
    int n = 0;
    std::string missing;
    for (int i = 1; i < kBoneCount; ++i) {
        const int32_t idx = poseable_->GetBoneIndex(g_boneNames[i]);
        boneIndex_[i] = static_cast<int16_t>(idx >= 0 && idx < 0x7FFF ? idx : -1);
        if (boneIndex_[i] >= 0) {
            found[n++] = Entry{idx, static_cast<Bone>(i)};
        } else {
            if (!missing.empty()) missing += ", ";
            missing += BoneName(static_cast<Bone>(i));
        }
    }
    // UE reference skeletons store every parent before its children, so sorting by bone index gives parent -> child.
    std::sort(found, found + n, [](const Entry& a, const Entry& b) { return a.index < b.index; });
    for (int k = 0; k < n; ++k) order_[k] = found[k].bone;
    orderCount_ = n;
    if (!missing.empty()) QD_WARN(kTag, "bones not found (their offsets are skipped): {}", missing);
}

// The source keeps playing its single-node animation but must not be drawn. Both modes force
// VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones: with the engine default (AlwaysTickPose /
// OnlyTickPoseWhenRendered) an unrendered skeletal mesh stops refreshing its bone transforms, which is what makes
// "SetVisibility(false)" freeze the pose. URO is switched off too (it throttles unrendered meshes) and LOD0 is forced
// so LOD bone reduction cannot strip bones from the pose we copy.
void PoseDriver::HideSource() {
    source_->VisibilityBasedAnimTickOption = SDK::EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    source_->bEnableUpdateRateOptimizations = 0;
    source_->SetForcedLOD(1);   // 1 = LOD0, 0 = automatic
    hiddenWith_ = GetSourceHideMode();
    switch (hiddenWith_) {
        case SourceHideMode::CollapseBones:
            // Render-only: hidden bones get zero-scale skinning matrices, the component-space pose is untouched.
            source_->HideBoneByName(source_->GetBoneName(0), SDK::EPhysBodyOp::PBO_None);
            break;
        case SourceHideMode::HiddenInGame:
        default:
            source_->SetHiddenInGame(true, false);   // no propagation: the poseable is our child and must stay visible
            break;
    }
}

void PoseDriver::ShowSource() {
    switch (hiddenWith_) {
        case SourceHideMode::CollapseBones:
            source_->UnHideBoneByName(source_->GetBoneName(0));
            break;
        case SourceHideMode::HiddenInGame:
        default:
            source_->SetHiddenInGame(false, false);
            break;
    }
    source_->SetForcedLOD(0);
}

// ---------------------------------------------------------------------------------------------------------- attacks

void PoseDriver::BeginFade() {
    if (current_.empty()) return;
    fadeFrom_ = current_;   // what was on screen last frame (attack + any older fade)
    fadeLeft_ = kFadeTime;
}

void PoseDriver::PlayAttack(const std::string& itemPath, int variant) {
    if (!poseable_) return;
    if (!classified_ || itemPath != lastPath_) {
        lastPath_ = itemPath;
        class_ = ClassifyWeapon(itemPath);
        classified_ = true;
        QD_INFO(kTag, "weapon '{}' -> {}", itemPath, WeaponClassName(class_));
    }
    if (attacking_) BeginFade();
    if (const int fv = g_forceVariant.load(); fv >= 0) variant = fv;
    variant_ = ((variant % 3) + 3) % 3;
    duration_ = Duration(class_, variant_);
    elapsed_ = 0.0f;
    attacking_ = true;
    QD_DEBUG(kTag, "attack {} v{} ({:.2f} s)", MotionName(class_, variant_), variant_, duration_);
}

void PoseDriver::StopAttack() {
    if (!attacking_) return;
    BeginFade();
    attacking_ = false;
}

namespace {
std::mutex g_tuneMutex;
// Baked hand corrections from in-game side-view tuning: thrusts need the weapon laid level.
ClassTune MakeDefaults(int cls) {
    ClassTune t;
    switch (static_cast<WeaponClass>(cls)) {
        case WeaponClass::Spear:  t.hand[0][0] = -65; t.hand[1][0] = -35; break;   // level thrust; high thrust a bit less
        case WeaponClass::Sword:  t.hand[2][0] = -65; break;                       // v2 = forward thrust
        case WeaponClass::Dagger: t.hand[0][0] = -55; t.hand[2][0] = -55; break;   // stab / double stab
        default: break;
    }
    return t;
}
ClassTune  g_tune[16] = {MakeDefaults(0), MakeDefaults(1), MakeDefaults(2), MakeDefaults(3), MakeDefaults(4),
                         MakeDefaults(5), MakeDefaults(6), MakeDefaults(7), MakeDefaults(8), MakeDefaults(9),
                         MakeDefaults(10), MakeDefaults(11), MakeDefaults(12), MakeDefaults(13), MakeDefaults(14),
                         MakeDefaults(15)};
}  // namespace

void SetClassTune(WeaponClass wc, const ClassTune& t) {
    std::lock_guard lock(g_tuneMutex);
    g_tune[static_cast<int>(wc) & 15] = t;
}
std::atomic<float> g_freezeT{-1.0f};
std::atomic<int>   g_forceVariant{-1};
void DebugFreeze(float t) { g_freezeT = t; }
void DebugForceVariant(int v) { g_forceVariant = v; }

ClassTune GetClassTune(WeaponClass wc) {
    std::lock_guard lock(g_tuneMutex);
    return g_tune[static_cast<int>(wc) & 15];
}

void PoseDriver::Tick(float dt) {
    if (!poseable_) return;
    if (!ue::IsValid(poseable_) || !ue::IsValid(source_)) {
        QD_WARN(kTag, "poseable or source vanished, dropping the pose driver");
        Forget();
        return;
    }
    SDK::USkeletalMesh* mesh = source_->SkeletalMesh;
    if (!mesh) return;
    if (static_cast<const void*>(mesh) != mesh_) {   // someone swapped the source mesh: follow it
        poseable_->SetSkeletalMesh(mesh, true);
        mesh_ = mesh;
        ResolveBones();
    }
    // CopyPoseFromSkeletalComponent's fast path assumes both bone arrays have the same size (only a check() guards it).
    if (poseable_->SkeletalMesh != mesh) return;
    poseable_->CopyPoseFromSkeletalComponent(source_);

    if (!(dt > 0.0f) || !std::isfinite(dt)) dt = 0.0f;
    if (blendLeft_ > 0.0f && blendFrom_.size() == allBoneNames_.size()) {
        blendLeft_ -= dt;
        if (blendLeft_ > 0.0f) {
            float a = 1.0f - blendLeft_ / blendTime_;
            a = a * a * (3.0f - 2.0f * a);   // smoothstep
            const size_t n = allBoneNames_.size();
            blendLive_.resize(n);
            std::vector<SDK::FName> names(n);
            // Read every live transform first: writing a parent changes what a later Get of its children returns.
            for (size_t i = 0; i < n; ++i) {
                names[i] = ue::MakeName(allBoneNames_[i]);
                const SDK::FTransform t = poseable_->GetBoneTransformByName(names[i], SDK::EBoneSpaces::ComponentSpace);
                blendLive_[i] = Xf{{t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W},
                                   {t.Translation.X, t.Translation.Y, t.Translation.Z},
                                   {t.Scale3D.X, t.Scale3D.Y, t.Scale3D.Z}};
            }
            for (size_t i = 0; i < n; ++i) {   // skeleton order = parents first
                const Xf& f = blendFrom_[i];
                const Xf& l = blendLive_[i];
                const float dot = f.r[0] * l.r[0] + f.r[1] * l.r[1] + f.r[2] * l.r[2] + f.r[3] * l.r[3];
                const float sign = dot < 0.0f ? -1.0f : 1.0f;
                float q[4];
                float len = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    q[k] = f.r[k] * (1.0f - a) + l.r[k] * sign * a;
                    len += q[k] * q[k];
                }
                len = std::sqrt(len);
                if (!(len > 1e-6f)) continue;
                SDK::FTransform t{};
                t.Rotation.X = q[0] / len;
                t.Rotation.Y = q[1] / len;
                t.Rotation.Z = q[2] / len;
                t.Rotation.W = q[3] / len;
                t.Translation.X = f.t[0] + (l.t[0] - f.t[0]) * a;
                t.Translation.Y = f.t[1] + (l.t[1] - f.t[1]) * a;
                t.Translation.Z = f.t[2] + (l.t[2] - f.t[2]) * a;
                t.Scale3D.X = f.s[0] + (l.s[0] - f.s[0]) * a;
                t.Scale3D.Y = f.s[1] + (l.s[1] - f.s[1]) * a;
                t.Scale3D.Z = f.s[2] + (l.s[2] - f.s[2]) * a;
                poseable_->SetBoneTransformByName(names[i], t, SDK::EBoneSpaces::ComponentSpace);
            }
        }
    }
    current_.clear();
    if (attacking_) {
        elapsed_ += dt;
        if (const float ft = g_freezeT.load(); ft >= 0.0f && duration_ > 0.0f) elapsed_ = std::min(ft, 0.999f) * duration_;
        const float t = duration_ > 0.0f ? elapsed_ / duration_ : 1.0f;
        if (t >= 1.0f) attacking_ = false;
        else {
            Sample(class_, variant_, t, current_);
            const ClassTune tune = GetClassTune(class_);
            if (tune.amp != 1.0f) for (auto& o : current_) { o.pitch *= tune.amp; o.yaw *= tune.amp; o.roll *= tune.amp; }
            const float* hf = tune.hand[variant_ % 3];
            if (hf[0] != 0.0f || hf[1] != 0.0f || hf[2] != 0.0f) {
                const float w = std::sin(3.14159265f * t);
                bool found = false;
                for (auto& o : current_) {
                    if (o.bone && std::strcmp(o.bone, "Hand_R") == 0) {
                        o.pitch += hf[0] * w; o.yaw += hf[1] * w; o.roll += hf[2] * w; found = true;
                    }
                }
                if (!found) current_.push_back(BoneOffset{"Hand_R", hf[0] * w, hf[1] * w, hf[2] * w});
            }
        }
    }
    if (fadeLeft_ > 0.0f) {
        fadeLeft_ -= dt;
        if (fadeLeft_ > 0.0f) AddScaled(current_, fadeFrom_, fadeLeft_ / kFadeTime);
        else fadeFrom_.clear();
    }
    if (!current_.empty()) ApplyOffsets(current_);
}

// Layering, parent -> child. UPoseableMeshComponent keeps LOCAL (parent-relative) bone transforms; a component-space
// Set converts the given transform into the parent's current component space and stores it as local. So writing a
// parent moves its whole subtree, and the next Get of a child already reflects the parent's new rotation: offsets
// accumulate down the chain, which is why bones must be processed in skeleton order.
void PoseDriver::ApplyOffsets(const std::vector<BoneOffset>& offsets) {
    const BoneOffset* byBone[kBoneCount] = {};
    for (const BoneOffset& o : offsets) {
        const Bone b = BoneFromName(o.bone ? o.bone : "");
        if (b != Bone::None) byBone[static_cast<int>(b)] = &o;
    }
    for (int k = 0; k < orderCount_; ++k) {
        const int bi = static_cast<int>(order_[k]);
        const BoneOffset* o = byBone[bi];
        if (!o || std::fabs(o->pitch) + std::fabs(o->yaw) + std::fabs(o->roll) < 0.01f) continue;
        const SDK::FName& name = g_boneNames[bi];
        SDK::FTransform t = poseable_->GetBoneTransformByName(name, SDK::EBoneSpaces::ComponentSpace);
        const Quat cur{t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W};
        const Quat q = Normalize(Mul(ToComponentSpace(*o, meshForwardYaw_), cur));   // rotate about the bone's own pivot
        t.Rotation.X = q.x;
        t.Rotation.Y = q.y;
        t.Rotation.Z = q.z;
        t.Rotation.W = q.w;
        poseable_->SetBoneTransformByName(name, t, SDK::EBoneSpaces::ComponentSpace);
    }
}

}  // namespace qd::motion
