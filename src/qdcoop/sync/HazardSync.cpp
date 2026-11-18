// Knights' Domain: enemy projectiles and area attacks. See HazardSync.h.
//
// Enemy attacks that are more than a swing spawn an actor from the enemy's Blueprint (SpawnActor node =
// GameplayStatics.BeginDeferredActorSpawnFromClass + FinishSpawningActor). Only the enemy's owner runs that
// Blueprint, so only the owner had the arrow / fireball / puddle. Here the owner watches FinishSpawningActor; when the
// calling script belongs to an enemy it owns, it sends the class, transform and the actor's plain variables. The
// others spawn the same class with their own copy of the enemy as Owner / Instigator. What such an actor spawns in
// turn (a fireball's puddle) is not sent: every copy of the fireball spawns its own.
//
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/SavableActor_classes.hpp"
#include "SDK/CoreItemPickupActor_classes.hpp"
#include "SDK/FBAttributes_structs.hpp"

#include "qdcoop/sync/HazardSync.h"
#include "qdcoop/sync/WorldSync.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/EnemySync.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace qd::sync {
namespace {

constexpr const char* kTag = "hazard";
constexpr uint8_t kOwnerIsEnemy = 1, kInstigatorIsEnemy = 2, kIsDrop = 4;
constexpr const char* kTrySpawningItem = "/Game/Blueprints/ItemSystem/SpawnItemDrop.SpawnItemDrop_C:TrySpawningItem";
constexpr const char* kPickupClass = "/Game/Blueprints/CoreItemPickupActor.CoreItemPickupActor_C";
constexpr size_t kDropperOffset = 0x90;   // SpawnItemDrop_C_TrySpawningItem::Dropper
constexpr size_t kLocationOffset = 0x70;  // SpawnItemDrop_C_TrySpawningItem::Location
constexpr double kRollWindowSec = 4.0;     // a drop pickup this soon after a roll ...
constexpr float  kRollRadiusCm = 600.0f;   // ... and this close to where it was rolled belongs to it

double NowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr uint8_t kRefNone = 0, kRefEnemy = 1;

struct Field {
    int32_t offset = 0;
    uint8_t size = 0;      // bytes in the object (bool: 1; damage set: 4 = its DamageAmount)
    uint8_t mask = 0;      // != 0: bool bit mask
    bool    isRef = false; // object pointer: one byte on the wire (kRefNone / kRefEnemy)
};
struct Layout {
    uint32_t hash = 0;
    size_t   bytes = 0;    // on the wire
    std::vector<Field> fields;
};

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    bool spawningRemote = false;
    SDK::UClass* pickupCls = nullptr;
    // loot rolls made on this machine (enemies we run, chests / pots opened here; enemy id 0 = not an enemy). The
    // pickup appears a moment after the roll (the item class loads first), so new drop pickups near a recent roll
    // are matched to it and sent.
    struct Roll { double at; uint64_t enemy; SDK::FVector where; };
    struct Recent { double at; SDK::UClass* cls; SDK::FVector where; };
    std::vector<Recent> sentDrops;            // drops we rolled and sent: a twin from another machine is not spawned
    std::vector<Roll> rolls;
    std::unordered_map<const void*, bool> knownDrops;   // drop pickups already seen (sent or received)
    SDK::UWorld* dropWorld = nullptr;
    std::vector<net::MsgHazard> outbox;      // filled inside the spawn hook, sent by the ticker
    std::vector<net::MsgHazard> inbox;       // received, spawned by the ticker (never inside a hook)
    std::unordered_map<const void*, Layout> layouts;
    int sent = 0, spawned = 0;
};
State& S() {
    static State s;
    return s;
}

bool GeneratedName(const std::string& n) {   // timeline outputs: they belong to a running timeline
    if (n.find("__Direction_") != std::string::npos) return true;
    if (n.size() > 33 && n[n.size() - 33] == '_') {
        bool hex = true;
        for (size_t i = n.size() - 32; i < n.size() && hex; ++i) hex = std::isxdigit(static_cast<unsigned char>(n[i])) != 0;
        if (hex) return true;
    }
    return false;
}

// The variables a Blueprint declares below AActor: bool / int / float / byte / small enum, vectors and rotators,
// and object pointers (only "points at the enemy" is carried).
const Layout& LayoutFor(SDK::UClass* cls) {
    auto& cache = S().layouts;
    if (auto it = cache.find(cls); it != cache.end()) return it->second;
    Layout l;
    uint64_t h = 1469598103934665603ull;
    SDK::UClass* actorCls = SDK::AActor::StaticClass();
    int depth = 0;
    for (SDK::UStruct* st = cls; st && st != actorCls && depth < 16; st = st->SuperStruct, ++depth) {
        // native classes keep engine state in their properties: only Blueprint-declared variables are copied
        if (ue::PathName(st).rfind("/Script/", 0) == 0) break;
        for (SDK::FField* f = st->ChildProperties; f; f = f->Next) {
            if (!f->ClassPrivate) continue;
            const std::string type = ue::ToString(f->ClassPrivate->Name);
            auto* p = static_cast<SDK::FProperty*>(f);
            if (p->ArrayDim != 1 || p->Offset < 0) continue;
            Field fd;
            size_t wire = 0;
            if (type == "BoolProperty") {
                auto* b = static_cast<SDK::FBoolProperty*>(f);
                if (b->ByteMask == 0) continue;
                fd.offset = p->Offset + b->ByteOffset;
                fd.size = 1;
                fd.mask = b->ByteMask;
                wire = 1;
            } else if (type == "IntProperty" || type == "FloatProperty") {
                fd.offset = p->Offset;
                fd.size = 4;
                wire = 4;
            } else if (type == "ByteProperty" || (type == "EnumProperty" && p->ElementSize == 1)) {
                fd.offset = p->Offset;
                fd.size = 1;
                wire = 1;
            } else if (type == "StructProperty") {
                auto* sp = static_cast<SDK::FStructProperty*>(f);
                const std::string sn = sp->Struct ? ue::Name(sp->Struct) : std::string();
                if (sn == "FB_DamageAttributeSet") {
                    // set by the attack when it spawns the actor (ExposeOnSpawn). Its tag containers are engine
                    // allocations and stay at the class defaults here; the amount (first member) is a plain float.
                    fd.offset = p->Offset + static_cast<int32_t>(offsetof(SDK::FFB_DamageAttributeSet, DamageAmount));
                    fd.size = 4;
                    wire = 4;
                } else if ((sn == "Vector" || sn == "Rotator") && p->ElementSize == 12) {
                    fd.offset = p->Offset;
                    fd.size = 12;
                    wire = 12;
                } else {
                    continue;
                }
            } else if (type == "ObjectProperty") {
                // components the Blueprint created itself are also object variables: never touch those
                auto* op = static_cast<SDK::FObjectPropertyBase*>(f);
                if (!op->PropertyClass || op->PropertyClass->IsSubclassOf(SDK::UActorComponent::StaticClass())) continue;
                fd.offset = p->Offset;
                fd.size = 8;
                fd.isRef = true;
                wire = 1;
            } else {
                continue;
            }
            const std::string name = ue::ToString(f->Name);
            if (GeneratedName(name)) continue;
            l.fields.push_back(fd);
            l.bytes += wire;
            for (const char c : name) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
            h = (h ^ (static_cast<uint64_t>(fd.size) | (fd.mask ? 0x100u : 0u) | (fd.isRef ? 0x200u : 0u))) * 1099511628211ull;
        }
    }
    if (l.bytes > net::kMaxHazardVarBytes) {
        l.fields.clear();
        l.bytes = 0;
    }
    l.hash = static_cast<uint32_t>(h ^ (h >> 32));
    return cache.emplace(cls, std::move(l)).first->second;
}

void Capture(SDK::AActor* a, const Layout& l, const SDK::AActor* enemy, std::vector<uint8_t>& out) {
    out.resize(l.bytes);
    const uint8_t* obj = reinterpret_cast<const uint8_t*>(a);
    size_t o = 0;
    for (const Field& f : l.fields) {
        if (f.isRef) {
            const void* v = nullptr;
            std::memcpy(&v, obj + f.offset, 8);
            out[o++] = v == enemy ? kRefEnemy : kRefNone;
        } else if (f.mask) {
            out[o++] = (obj[f.offset] & f.mask) ? 1 : 0;
        } else {
            std::memcpy(&out[o], obj + f.offset, f.size);
            o += f.size;
        }
    }
}

void Apply(SDK::AActor* a, const Layout& l, SDK::AActor* enemy, const std::vector<uint8_t>& data) {
    if (data.size() != l.bytes) return;
    uint8_t* obj = reinterpret_cast<uint8_t*>(a);
    size_t o = 0;
    for (const Field& f : l.fields) {
        if (f.isRef) {
            if (data[o++] == kRefEnemy && enemy) std::memcpy(obj + f.offset, &enemy, 8);
        } else if (f.mask) {
            if (data[o++]) obj[f.offset] |= f.mask;
            else obj[f.offset] &= static_cast<uint8_t>(~f.mask);
        } else {
            if (f.size == 4 || f.size == 12) {   // floats inside: refuse NaN / infinity
                bool ok = true;
                for (int i = 0; i < f.size; i += 4) {
                    float v = 0;
                    std::memcpy(&v, &data[o + i], 4);
                    uint32_t bits = 0;
                    std::memcpy(&bits, &v, 4);
                    if ((bits & 0x7F800000u) == 0x7F800000u) ok = false;
                }
                if (ok) std::memcpy(obj + f.offset, &data[o], f.size);
            } else {
                std::memcpy(obj + f.offset, &data[o], f.size);
            }
            o += f.size;
        }
    }
}

// The registered enemy this script object belongs to: the enemy itself or something it owns (component, anim instance).
uint64_t EnemyOfCaller(SDK::UObject* caller) {
    int depth = 0;
    for (SDK::UObject* o = caller; o && depth < 4; o = o->Outer, ++depth) {
        if (!o->IsA(SDK::AActor::StaticClass())) continue;
        return EnemyIdOfActor(static_cast<SDK::AActor*>(o));
    }
    return 0;
}

// Runs after every FinishSpawningActor made from a script, on the game thread.
void OnSpawned(SDK::UObject* caller, SDK::UFunction*, void* result) {
    auto& s = S();
    if (s.spawningRemote || !result || !caller || !coop::InSession()) return;
    SDK::AActor* actor = *static_cast<SDK::AActor**>(result);
    if (!actor || !ue::IsValid(actor) || !actor->Class) return;
    if (!ue::IsValid(s.pickupCls)) s.pickupCls = ue::FindClass(kPickupClass);
    if (s.pickupCls && actor->IsA(s.pickupCls)) return;   // loot: matched to its roll by ScanDrops
    uint64_t id = EnemyOfCaller(caller);
    // spawned from an anim notify or a component's script: the caller is not the enemy, the new actor's
    // Instigator / Owner usually is
    if (id == 0 && actor->Instigator) id = EnemyIdOfActor(actor->Instigator);
    if (id == 0 && actor->Owner) id = EnemyIdOfActor(actor->Owner);
    if (id == 0 || !EnemyOwnedHere(id)) return;
    // not attacks: other enemies (summons have their own sync), players, and saved world actors (loot is sent as a drop)
    if (actor->IsA(SDK::APawn::StaticClass())) return;
    if (SDK::UClass* sav = SDK::ASavableActor_C::StaticClass(); sav && actor->IsA(sav)) return;
    SDK::AActor* enemy = EnemyActorOfId(id);
    if (!enemy) return;
    const auto slot = coop::LocalSlot();
    if (!slot) return;

    net::MsgHazard m;
    m.enemyId = id;
    m.ownerSlot = *slot;
    m.classPath = ue::PathName(actor->Class);
    if (m.classPath.empty() || m.classPath.size() > net::kMaxClassPathBytes) return;
    if (actor->Owner == enemy) m.flags |= kOwnerIsEnemy;
    if (static_cast<SDK::AActor*>(actor->Instigator) == enemy) m.flags |= kInstigatorIsEnemy;
    const SDK::FVector loc = actor->K2_GetActorLocation();
    const SDK::FRotator rot = actor->K2_GetActorRotation();
    const SDK::FVector sc = actor->GetActorScale3D();
    m.pos = {loc.X, loc.Y, loc.Z};
    m.rot = {rot.Pitch, rot.Yaw, rot.Roll};
    m.scale = {sc.X, sc.Y, sc.Z};
    const Layout& l = LayoutFor(actor->Class);
    m.layout = l.hash;
    if (!l.fields.empty()) Capture(actor, l, enemy, m.vars);
    s.outbox.push_back(std::move(m));
}

bool IsDropPickup(SDK::AActor* a) {
    auto& s = S();
    if (!ue::IsValid(s.pickupCls)) s.pickupCls = ue::FindClass(kPickupClass);
    return s.pickupCls && a && ue::IsValid(a) && a->IsA(s.pickupCls) &&
           static_cast<SDK::ACoreItemPickupActor_C*>(a)->IsADrop;
}

// Owner: send every new drop pickup that belongs to a recent roll of one of our enemies.
void ScanDrops(double now) {
    auto& s = S();
    SDK::UWorld* w = ue::World();
    if (w != s.dropWorld) {
        s.dropWorld = w;
        s.knownDrops.clear();
    }
    std::erase_if(s.rolls, [&](const State::Roll& r) { return now - r.at > kRollWindowSec; });
    std::erase_if(s.sentDrops, [&](const State::Recent& r) { return now - r.at > kRollWindowSec * 2; });
    if (s.rolls.empty() || !ue::IsValid(s.pickupCls)) return;
    std::vector<SDK::AActor*> found;
    ActorsOfClass(s.pickupCls, found);
    const auto slot = coop::LocalSlot();
    if (!slot) return;
    for (SDK::AActor* a : found) {
        if (s.knownDrops.count(a) || !ue::IsValid(a)) continue;
        // spawned at run time: "<Class>_C_<number>". Pickups placed in a level ("Herb_Item_BP_2") that stream in
        // near a kill are not its loot.
        if (ue::Name(a).rfind(ue::Name(a->Class) + "_", 0) != 0) {
            s.knownDrops[a] = true;
            continue;
        }
        const SDK::FVector at = a->K2_GetActorLocation();
        const State::Roll* best = nullptr;
        float bestD = kRollRadiusCm;
        for (const auto& r : s.rolls) {
            const float dx = at.X - r.where.X, dy = at.Y - r.where.Y, dz = at.Z - r.where.Z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d <= bestD) { bestD = d; best = &r; }
        }
        if (!best) continue;
        s.knownDrops[a] = true;
        net::MsgHazard m;
        m.enemyId = best->enemy;
        m.ownerSlot = *slot;
        m.flags = kIsDrop;
        m.classPath = ue::PathName(a->Class);
        if (m.classPath.empty() || m.classPath.size() > net::kMaxClassPathBytes) continue;
        const SDK::FRotator rot = a->K2_GetActorRotation();
        const SDK::FVector sc = a->GetActorScale3D();
        m.pos = {at.X, at.Y, at.Z};
        m.rot = {rot.Pitch, rot.Yaw, rot.Roll};
        m.scale = {sc.X, sc.Y, sc.Z};
        auto* sa = static_cast<SDK::ASavableActor_C*>(a);
        if (sa->GUIDs.Num() >= 1) {
            const SDK::FGuid& g = sa->GUIDs[0];
            m.guid = {static_cast<uint32_t>(g.A), static_cast<uint32_t>(g.B), static_cast<uint32_t>(g.C),
                      static_cast<uint32_t>(g.D)};
        }
        const Layout& l = LayoutFor(a->Class);
        m.layout = l.hash;
        if (!l.fields.empty()) Capture(a, l, nullptr, m.vars);
        QD_INFO(kTag, "drop {} matched to roll of {:016X} ({:.0f} cm away)", m.classPath, best->enemy, bestD);
        s.sentDrops.push_back({now, a->Class, at});
        s.outbox.push_back(std::move(m));
    }
}

void SpawnRemote(const net::MsgHazard& m) {
    auto& s = S();
    SDK::UWorld* w = ue::World();
    SDK::UClass* cls = ue::FindClass(m.classPath);
    SDK::AActor* enemy = EnemyActorOfId(m.enemyId);
    const bool isDrop = (m.flags & kIsDrop) != 0;
    if (!w || !cls) return;
    if (!enemy && !isDrop) return;   // an attack of an enemy that is not loaded here: nothing to show
    if (isDrop) {
        const double now = NowSec();
        for (const auto& r : s.sentDrops) {
            const float dx = m.pos.x - r.where.X, dy = m.pos.y - r.where.Y, dz = m.pos.z - r.where.Z;
            if (r.cls == cls && now - r.at < kRollWindowSec * 2 && dx * dx + dy * dy + dz * dz < 200.f * 200.f) {
                QD_INFO(kTag, "drop {} skipped: we rolled the same one here", m.classPath);
                return;
            }
        }
    }
    if (!cls->IsSubclassOf(SDK::AActor::StaticClass()) || cls->IsSubclassOf(SDK::APawn::StaticClass())) return;
    for (float v : {m.pos.x, m.pos.y, m.pos.z, m.rot.x, m.rot.y, m.rot.z, m.scale.x, m.scale.y, m.scale.z})
        if (!std::isfinite(v)) return;

    SDK::FTransform t{};
    const SDK::FRotator rot(m.rot.x, m.rot.y, m.rot.z);
    t.Rotation = SDK::UKismetMathLibrary::Conv_RotatorToTransform(rot).Rotation;
    t.Translation = SDK::FVector(m.pos.x, m.pos.y, m.pos.z);
    t.Scale3D = SDK::FVector(m.scale.x, m.scale.y, m.scale.z);
    s.spawningRemote = true;
    SDK::AActor* a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        w, cls, t, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        (enemy && (m.flags & kOwnerIsEnemy)) ? enemy : nullptr);
    if (a) {
        if (isDrop) s.knownDrops[a] = true;
        if (enemy && (m.flags & kInstigatorIsEnemy) && enemy->IsA(SDK::APawn::StaticClass()))
            a->Instigator = static_cast<SDK::APawn*>(enemy);
        const Layout& l = LayoutFor(cls);
        if (l.hash == m.layout && !l.fields.empty()) Apply(a, l, enemy, m.vars);
        const bool drop = (m.flags & kIsDrop) && (m.guid[0] | m.guid[1] | m.guid[2] | m.guid[3]) != 0 &&
                          a->IsA(SDK::ASavableActor_C::StaticClass());
        auto setGuid = [&] {
            auto* sa = static_cast<SDK::ASavableActor_C*>(a);
            if (sa->GUIDs.Num() < 1) return false;
            SDK::FGuid& g = sa->GUIDs[0];
            g.A = m.guid[0]; g.B = m.guid[1]; g.C = m.guid[2]; g.D = m.guid[3];
            return true;
        };
        if (drop) setGuid();
        SDK::UGameplayStatics::FinishSpawningActor(a, t);
        if (drop && !setGuid()) QD_WARN(kTag, "drop {} spawned without a GUID slot", m.classPath);
        if (drop) {
            // the owner's TrySpawningItem wrote its ItemDrops entry; ours was skipped, so the pickup writes it now
            // (only drops the game saves: coins and pot loot are not kept in the save)
            if (SDK::UFunction* fn = IsDropPickup(a) ? ue::FindFunction(a, "SaveDropData") : nullptr) {
                alignas(16) uint8_t parms[64] = {};
                ue::CallFunction(a, fn, parms);
            }
            QD_INFO(kTag, "drop {} spawned and saved with the owner's GUID {:08X}...", m.classPath, m.guid[0]);
        }
        ++s.spawned;
    }
    s.spawningRemote = false;
}

void OnHazard(uint8_t fromSlot, const uint8_t* data, size_t size) {
    net::MsgHazard m;
    if (!net::Decode(data, size, m)) return;
    if (coop::IsHost()) {
        m.ownerSlot = fromSlot;
        const std::vector<uint8_t> bytes = net::Encode(m);
        for (const coop::Member& mem : coop::Members())
            if (mem.present && !mem.isLocal && mem.slot != fromSlot) coop::SendToSlot(mem.slot, net::Channel::Reliable, bytes);
    } else if (fromSlot != 0) {
        return;
    }
    if (!(m.flags & kIsDrop) && EnemyOwnedHere(m.enemyId)) return;   // ownership moved to us: our copy spawns its own
    S().inbox.push_back(std::move(m));
}

void Tick(float) {
    auto& s = S();
    if (!coop::InSession()) {
        s.outbox.clear();
        s.inbox.clear();
        return;
    }
    if (!s.rolls.empty()) ScanDrops(NowSec());
    if (!s.outbox.empty()) {
        std::vector<net::MsgHazard> out;
        out.swap(s.outbox);
        for (const net::MsgHazard& m : out) {
            coop::Broadcast(net::Channel::Reliable, net::Encode(m));
            if (++s.sent <= 20 || s.sent % 100 == 0) QD_INFO(kTag, "sent {} (#{})", m.classPath, s.sent);
        }
    }
    if (!s.inbox.empty()) {
        std::vector<net::MsgHazard> in;
        in.swap(s.inbox);
        for (const net::MsgHazard& m : in) {
            const int before = s.spawned;
            SpawnRemote(m);
            if (s.spawned != before && (s.spawned <= 20 || s.spawned % 100 == 0))
                QD_INFO(kTag, "spawned {} from slot {} (#{})", m.classPath, m.ownerSlot, s.spawned);
        }
    }
}

}  // namespace

void HazardSyncInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    coop::On(net::MsgType::Hazard, &OnHazard);
    // Loot: the enemy's owner rolls it. Others skip their own roll and get the owner's drop through OnSpawned.
    hooks::AddPre(kTrySpawningItem, [](hooks::CallInfo& ci) {
        auto& st = S();
        if (!coop::InSession() || !ci.params) return false;
        SDK::AActor* dropper = *reinterpret_cast<SDK::AActor**>(static_cast<uint8_t*>(ci.params) + kDropperOffset);
        const bool valid = dropper && ue::IsValid(dropper);
        const uint64_t id = valid ? EnemyIdOfActor(dropper) : 0;
        static int logged = 0;
        if (logged++ < 40)
            QD_INFO(kTag, "loot roll by {} (enemy {:016X}, {})", valid ? ue::ClassName(dropper) : std::string("none"), id,
                    id != 0 && !EnemyOwnedHere(id)                                  ? "skipped: the enemy's owner sends it"
                    : WorldSyncReplaying() || (valid && WorldSyncReplayedRecently(dropper, 15.0)) ? "skipped: broken by another player, who sends it"
                                                                                                   : "rolled here and sent");
        // an enemy someone else runs: that machine rolls and sends it. Anything else (chests, pots ...) only rolls on
        // the machine where it was opened, and is sent from there.
        if (id != 0 && !EnemyOwnedHere(id)) return true;
        // a pot broken on another machine: its drop comes from there (the roll comes at the end of the break
        // animation, after the replay itself)
        if (WorldSyncReplaying() || (valid && WorldSyncReplayedRecently(dropper, 15.0))) return true;
        const auto* loc = reinterpret_cast<const SDK::FVector*>(static_cast<uint8_t*>(ci.params) + kLocationOffset);
        st.rolls.push_back({NowSec(), id, *loc});
        // pickups that exist before the roll are not its loot (coins and pot loot are not flagged as saved drops,
        // so "new since the roll" is what tells them apart)
        if (!ue::IsValid(st.pickupCls)) st.pickupCls = ue::FindClass(kPickupClass);
        if (st.pickupCls) {
            std::vector<SDK::AActor*> existing;
            ActorsOfClass(st.pickupCls, existing);
            for (SDK::AActor* a : existing) st.knownDrops[a] = true;
        }
        return false;
    });
    if (!hooks::AddNativePost("/Script/Engine.GameplayStatics:FinishSpawningActor", &OnSpawned))
        QD_WARN(kTag, "FinishSpawningActor not hooked: enemy projectiles stay local");
    s.ticker = gamethread::AddTicker("hazardsync", [](float dt) { Tick(dt); });
    QD_INFO(kTag, "initialized");
}

void HazardSyncShutdown() {
    auto& s = S();
    if (!s.inited) return;
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    s.inited = false;
}

}  // namespace qd::sync
