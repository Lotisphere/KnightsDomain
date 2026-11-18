// Knights' Domain: shared world state. See WorldSync.h.
//
// The game keeps every persistent world change in the active save slot:
//   SavableActorStates  GUID -> int   chests, doors, levers, lifts, secret walls, barricades, pickups, quest managers
//   NamedSaveData       name -> int   story counters and flags
//   SavePointsVisited / UnlockedTeleporters / LastTouchedCheckpoint / SavePointID
// This module mirrors those between the players. Four times a second it compares the local save with what the
// session already agreed on ("known") and sends the entries that changed here; received entries are written to the
// local save through the game's own Blueprint functions and the live actor is told to restore itself from them.
//
// Wire: MsgWorldFlags. FlagValue::name is a one-letter kind followed by the key:
//   g<32 hex>  savable actor state        n<name>  named save data        v<path>  save point visited
//   t<path>    teleporter unlocked        c<path>  last touched checkpoint (value unused)   s<path>  save point id
//   x<32 hex>  a loot drop was picked up (its ItemDrops entry went away): everyone removes it
//   e<n>:<path> event, not state: Blueprint function kMirrored[n] ran on that level object here; every other machine
//               runs it on its own copy (cutscenes, activation triggers). Never remembered or resent.
// Loot drops themselves are spawned by HazardSync (the enemy's owner rolls them once).
// Joining: the host sends its whole state (full = true). A client keeps quiet until that arrived, takes it, and then
// sends only what the host does not have, so a client's older save never overwrites the host's world.
// Host relays every client change to the other clients. Last writer wins.
//
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/FS_SaveSlot_classes.hpp"
#include "SDK/SavableActor_classes.hpp"
#include "SDK/SavableActor_parameters.hpp"
#include "SDK/SavableActorState_structs.hpp"
#include "SDK/ItemPickup_Base_classes.hpp"
#include "SDK/SaveFunctions_classes.hpp"
#include "SDK/SaveFunctions_parameters.hpp"

#include "qdcoop/sync/WorldSync.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace qd::sync {
namespace {

constexpr const char* kTag = "worldsync";
constexpr float  kPollSec = 0.25f;
constexpr float  kIndexSec = 1.0f;       // how often the GUID -> live actor index is rebuilt
constexpr size_t kFlagsPerMsg = 300;     // ~40 bytes each; the reliable channel carries 64 KiB

using Guid = std::array<uint32_t, 4>;

struct Snapshot {
    std::map<Guid, int32_t>        states;
    std::map<std::string, int32_t> named;
    std::set<std::string>          visited, teleporters;
    std::string                    lastCheckpoint, savePoint;
    std::set<Guid>                 drops;   // ItemDrops keys (loot on the ground that the save remembers)
};

struct LiveRef { SDK::ASavableActor_C* actor = nullptr; int index = 0; };

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    bool     inSession = false;
    bool     haveFull = false;           // client: the host's full state has arrived
    Snapshot known;                      // what the session agrees on (as far as this machine knows)
    SDK::UObject* lastSave = nullptr;    // save object the last poll looked at (a reload replaces it)
    SDK::UWorld*  lastWorld = nullptr;
    float    pollIn = 0, indexIn = 0;
    std::map<Guid, std::vector<LiveRef>> live;
    bool     pending = false;            // a received entry could not be written yet
    std::set<Guid> taken;                // loot drops picked up by anyone this session
    std::set<Guid> dropsSeen;            // ItemDrops keys at the last poll (same save object)
    std::vector<std::string> eventsOut;  // mirrored events that ran here, sent by the ticker
    double   quietUntil = 0;             // no mirrored events before this (a save or world is being restored)
    bool replaying = false;              // inside a mirrored event received from another machine
    std::unordered_map<const void*, double> replayed;   // objects a received event ran on, and when
};
State& S() {
    static State s;
    return s;
}

// The SDK's TArray keeps its fields protected; same layout.
struct RawArray { void* data; int32_t num; int32_t max; };

// ------------------------------------------------------------------ keys

Guid ToGuid(const SDK::FGuid& g) {
    return {static_cast<uint32_t>(g.A), static_cast<uint32_t>(g.B), static_cast<uint32_t>(g.C), static_cast<uint32_t>(g.D)};
}
SDK::FGuid FromGuid(const Guid& g) {
    SDK::FGuid o{};
    o.A = g[0]; o.B = g[1]; o.C = g[2]; o.D = g[3];
    return o;
}
std::string GuidKey(const Guid& g, char kind = 'g') {
    char b[40];
    std::snprintf(b, sizeof b, "%c%08X%08X%08X%08X", kind, g[0], g[1], g[2], g[3]);
    return b;
}
bool ParseGuid(const std::string& hex, Guid& out) {
    if (hex.size() != 32) return false;
    for (int i = 0; i < 4; ++i) {
        unsigned v = 0;
        if (std::sscanf(hex.substr(i * 8, 8).c_str(), "%8X", &v) != 1) return false;
        out[i] = v;
    }
    return true;
}

// ------------------------------------------------------------------ game access

SDK::UFS_SaveSlot_C* ActiveSave() {
    SDK::UWorld* w = ue::World();
    if (!w || !ue::IsValid(w->OwningGameInstance)) return nullptr;
    SDK::UGameInstance* gi = w->OwningGameInstance;
    if (!gi->IsA(SDK::UFS_GameInstanceBase::StaticClass())) return nullptr;
    auto* base = static_cast<SDK::UFS_GameInstanceBase*>(gi);
    if (base->bLoadingMap) return nullptr;
    SDK::UFS_SaveGame* save = base->BaseActiveSave;
    SDK::UClass* slotCls = SDK::UFS_SaveSlot_C::StaticClass();
    if (!save || !ue::IsValid(save) || !slotCls || !save->IsA(slotCls)) return nullptr;
    return static_cast<SDK::UFS_SaveSlot_C*>(save);
}

std::string LocPath(const SDK::UFS_LocationData* l) { return l && ue::IsValid(l) ? ue::PathName(l) : std::string(); }

SDK::UFS_LocationData* FindLoc(const std::string& path) {
    if (path.empty()) return nullptr;
    SDK::UObject* o = ue::FindObject(path);
    if (!o) o = ue::LoadObject(path);
    if (!o || !o->IsA(SDK::UFS_LocationData::StaticClass())) return nullptr;
    return static_cast<SDK::UFS_LocationData*>(o);
}

Snapshot Read(SDK::UFS_SaveSlot_C* s) {
    Snapshot o;
    for (auto& kv : s->SavableActorStates) o.states[ToGuid(kv.Key())] = kv.Value();
    for (auto& kv : s->NamedSaveData) o.named[ue::ToString(kv.Key())] = kv.Value();
    for (SDK::UFS_LocationData* l : s->SavePointsVisited)
        if (auto p = LocPath(l); !p.empty()) o.visited.insert(p);
    for (SDK::UFS_LocationData* l : s->UnlockedTeleporters)
        if (auto p = LocPath(l); !p.empty()) o.teleporters.insert(p);
    for (auto& kv : s->ItemDrops) o.drops.insert(ToGuid(kv.Key()));
    o.lastCheckpoint = LocPath(s->LastTouchedCheckpoint);
    o.savePoint = LocPath(s->SavePointID);
    return o;
}

// GUID -> loaded actors that own it. Actors of streamed-out sublevels are not in the world's levels.
void RebuildIndex() {
    auto& s = S();
    s.live.clear();
    SDK::UWorld* w = ue::World();
    SDK::UClass* cls = SDK::ASavableActor_C::StaticClass();
    if (!w || !cls) return;
    auto& objs = SDK::UObject::GObjects;
    for (int i = 0; i < objs->Num(); ++i) {
        SDK::UObject* o = objs->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        auto* a = static_cast<SDK::ASavableActor_C*>(o);
        if (a->bActorIsBeingDestroyed || !a->RootComponent) continue;
        SDK::UObject* outer = a->Outer;
        if (!outer || !outer->IsA(SDK::ULevel::StaticClass()) || static_cast<SDK::ULevel*>(outer)->OwningWorld != w) continue;
        int idx = 0;
        for (const SDK::FGuid& g : a->GUIDs) s.live[ToGuid(g)].push_back({a, idx++});
        // A restore run again on a loaded actor (the game's quest refresh after a story counter changes, or ours
        // after a shared change) appends to MySaveData instead of replacing it. Entries past GUIDs have no save slot
        // and are never read by the game; keep the list as a level load leaves it.
        auto* guids = reinterpret_cast<RawArray*>(&a->GUIDs);
        auto* data = reinterpret_cast<RawArray*>(&a->MySaveData);
        if (guids->num > 0 && data->num > guids->num) data->num = guids->num;
    }
}

// The game's own writer: SavableActor.Save_Data_Index_to_World_Save(Index, Data) stores Data under GUIDs[Index] in the
// active save and in the actor's MySaveData.
bool CallSaveIndex(SDK::ASavableActor_C* a, int index, int32_t value) {
    static SDK::UFunction* fn = nullptr;
    if (!ue::IsValid(fn)) fn = ue::FindFunction(a, "Save Data Index to World Save");
    if (!fn) fn = ue::FindFunction(a, "Save_Data_Index_to_World_Save");
    if (!fn) return false;
    SDK::Params::SavableActor_C_Save_Data_Index_to_World_Save p{};
    p.Index_0 = index;
    p.Data = value;
    ue::CallFunction(a, fn, &p);
    return true;
}

// The game's own path, as on a level load: RestoreFromSave reads the actor's GUIDs from the active save and runs
// "Restore State From Save Data" with a fresh array. (Handing it MySaveData itself made it append to the array it was
// reading: the client's chest ended up with two entries.) MySaveData never has more entries than GUIDs.
void CallRestore(SDK::ASavableActor_C* a) {
    static SDK::UFunction* fn = nullptr;
    if (!ue::IsValid(fn)) fn = ue::FindFunction(a, "RestoreFromSave");
    if (fn) {
        alignas(16) uint8_t parms[64] = {};
        ue::CallFunction(a, fn, parms);
    }
    auto* guids = reinterpret_cast<RawArray*>(&a->GUIDs);
    auto* data = reinterpret_cast<RawArray*>(&a->MySaveData);
    if (data->num > guids->num && guids->num >= 0) data->num = guids->num;
}

// Writes one savable state into the local save and shows it on the loaded actor(s). False = try again later.
bool ApplyState(const Guid& g, int32_t value) {
    auto& s = S();
    auto it = s.live.find(g);
    if (it != s.live.end() && !it->second.empty()) {
        bool ok = false;
        for (const LiveRef& r : it->second) {
            if (!ue::IsValid(r.actor)) continue;
            auto* raw = reinterpret_cast<RawArray*>(&r.actor->MySaveData);
            const bool had = r.index < raw->num;
            SDK::FSavableActorState before{};
            if (had) before = static_cast<SDK::FSavableActorState*>(raw->data)[r.index];
            if (!CallSaveIndex(r.actor, r.index, value)) continue;
            ok = true;
            raw = reinterpret_cast<RawArray*>(&r.actor->MySaveData);
            if (r.index < raw->num) {
                auto& slot = static_cast<SDK::FSavableActorState*>(raw->data)[r.index];
                slot.IsSet_1_52B3D8FF4C3CC8B0DAAD90BE9BA2A2B7 = true;
                slot.Value_4_81204DA949BB8DCF4D35D4B4419EC024 = value;
            }
            const bool same = had && before.IsSet_1_52B3D8FF4C3CC8B0DAAD90BE9BA2A2B7 &&
                              before.Value_4_81204DA949BB8DCF4D35D4B4419EC024 == value;
            if (!same) CallRestore(r.actor);
        }
        if (ok) return true;
    }
    // The owner is not loaded here: borrow any loaded savable actor to run the game's writer under this GUID.
    for (auto& [guid, refs] : s.live) {
        for (const LiveRef& r : refs) {
            if (r.index != 0 || !ue::IsValid(r.actor)) continue;
            auto* guids = reinterpret_cast<RawArray*>(&r.actor->GUIDs);
            auto* data = reinterpret_cast<RawArray*>(&r.actor->MySaveData);
            if (guids->num < 1 || data->num < 1) continue;
            SDK::FGuid& g0 = static_cast<SDK::FGuid*>(guids->data)[0];
            const SDK::FGuid keepGuid = g0;
            const SDK::FSavableActorState keepState = static_cast<SDK::FSavableActorState*>(data->data)[0];
            g0 = FromGuid(g);
            const bool ok = CallSaveIndex(r.actor, 0, value);
            static_cast<SDK::FGuid*>(reinterpret_cast<RawArray*>(&r.actor->GUIDs)->data)[0] = keepGuid;
            data = reinterpret_cast<RawArray*>(&r.actor->MySaveData);
            if (data->num >= 1) static_cast<SDK::FSavableActorState*>(data->data)[0] = keepState;
            if (ok) return true;
        }
    }
    return false;
}

bool ApplyNamed(const std::string& name, int32_t value) {
    SDK::UObject* cdo = SDK::USaveFunctions_C::GetDefaultObj();
    SDK::UWorld* w = ue::World();
    if (!cdo || !w) return false;
    static SDK::UFunction* fn = nullptr;
    if (!ue::IsValid(fn)) fn = ue::FindFunction(cdo, "SaveNamedData");
    if (!fn) return false;
    SDK::Params::SaveFunctions_C_SaveNamedData p{};
    p.Name_0 = ue::MakeName(name);
    p.Data = value;
    p.RefreshQuestStates = true;
    p.__WorldContext = w;
    ue::CallFunction(cdo, fn, &p);
    return true;
}

// Appends to a TArray<UFS_LocationData*> of the save. Growing needs memory from the engine's allocator, so the new
// buffer is one the engine itself handed out: the result array of GetAllActorsOfClass (same element size).
bool AppendLoc(SDK::TArray<SDK::UFS_LocationData*>& arr, SDK::UFS_LocationData* loc) {
    auto* raw = reinterpret_cast<RawArray*>(&arr);
    auto** items = static_cast<SDK::UFS_LocationData**>(raw->data);
    for (int i = 0; i < raw->num; ++i)
        if (items[i] == loc) return true;
    if (raw->num < raw->max) {
        items[raw->num++] = loc;
        return true;
    }
    SDK::UWorld* w = ue::World();
    SDK::UObject* cdo = SDK::UGameplayStatics::GetDefaultObj();
    static SDK::UFunction* fn = nullptr;
    if (!ue::IsValid(fn)) fn = ue::FindFunction(cdo, "GetAllActorsOfClass");
    if (!w || !cdo || !fn) return false;
    SDK::Params::GameplayStatics_GetAllActorsOfClass p{};
    p.WorldContextObject = w;
    p.ActorClass = SDK::AActor::StaticClass();
    ue::CallFunction(cdo, fn, &p);
    auto* got = reinterpret_cast<RawArray*>(&p.OutActors);
    if (!got->data || got->max <= raw->num) return false;
    auto** fresh = static_cast<SDK::UFS_LocationData**>(got->data);
    for (int i = 0; i < raw->num; ++i) fresh[i] = items[i];
    fresh[raw->num] = loc;
    const int n = raw->num + 1;
    // the old (small) buffer stays allocated: nothing here can hand it back to the engine
    raw->data = fresh;
    raw->num = n;
    raw->max = got->max;
    return true;
}

bool CallNoParams(SDK::UObject* o, const char* fn) {
    SDK::UFunction* f = ue::FindFunction(o, fn);
    if (!f) return false;
    alignas(16) uint8_t parms[64] = {};
    ue::CallFunction(o, f, parms);
    return true;
}

// A loot drop someone picked up: the loaded pickup with that GUID forgets its save entry and goes away. Not loaded
// here: any loaded pickup runs the game's DeleteDropData under that GUID, so the save forgets it too.
bool ApplyTaken(SDK::UFS_SaveSlot_C* save, const Guid& g) {
    auto& s = S();
    SDK::UClass* pick = SDK::AItemPickup_Base_C::StaticClass();
    bool inSave = false;
    for (auto& kv : save->ItemDrops)
        if (ToGuid(kv.Key()) == g) inSave = true;
    bool removed = false;
    if (auto it = s.live.find(g); it != s.live.end()) {
        for (const LiveRef& r : it->second) {
            if (r.index != 0 || !ue::IsValid(r.actor) || !pick || !r.actor->IsA(pick)) continue;
            CallNoParams(r.actor, "DeleteDropData");
            r.actor->K2_DestroyActor();
            removed = true;
        }
        if (removed) s.live.erase(it);
    }
    if (!removed && inSave && pick) {
        for (auto& [guid, refs] : s.live) {
            for (const LiveRef& r : refs) {
                if (r.index != 0 || !ue::IsValid(r.actor) || !r.actor->IsA(pick)) continue;
                auto* guids = reinterpret_cast<RawArray*>(&r.actor->GUIDs);
                if (guids->num < 1) continue;
                SDK::FGuid& g0 = static_cast<SDK::FGuid*>(guids->data)[0];
                const SDK::FGuid keep = g0;
                g0 = FromGuid(g);
                CallNoParams(r.actor, "DeleteDropData");
                static_cast<SDK::FGuid*>(reinterpret_cast<RawArray*>(&r.actor->GUIDs)->data)[0] = keep;
                return true;
            }
        }
        return false;   // nothing loaded to run it through: Reassert tries again
    }
    return true;
}

// Applies one received entry. False = could not be written yet (nothing loaded to write through).
bool ApplyEntry(SDK::UFS_SaveSlot_C* save, const std::string& name, int32_t value) {
    if (name.size() < 2) return true;
    const std::string key = name.substr(1);
    switch (name[0]) {
        case 'g': {
            Guid g{};
            if (!ParseGuid(key, g)) return true;
            return ApplyState(g, value);
        }
        case 'n': return ApplyNamed(key, value);
        case 'x': {
            Guid g{};
            if (!ParseGuid(key, g)) return true;
            return ApplyTaken(save, g);
        }
        case 'v': if (auto* l = FindLoc(key)) return AppendLoc(save->SavePointsVisited, l); return true;
        case 't': if (auto* l = FindLoc(key)) return AppendLoc(save->UnlockedTeleporters, l); return true;
        case 'c': if (auto* l = FindLoc(key)) save->LastTouchedCheckpoint = l; return true;
        case 's': if (auto* l = FindLoc(key)) save->SavePointID = l; return true;
        default: return true;
    }
}

// ------------------------------------------------------------------ mirrored events
// Level objects placed in the map have the same path on every machine. These Blueprint functions only ever run on
// the machine whose player caused them ("player 0" walked into the trigger, the boss the host runs died, the player
// talked to an NPC); the others run them on their own copy. Their lasting results (one-shot flags, story counters)
// are save data and travel the normal way.
constexpr const char* kMirrored[] = {
    // a trigger volume fires its targets: ambush spawners, gates, lifts, music, arena locks
    "/Game/Blueprints/ActivationTrigger.ActivationTrigger_C:DoTrigger",
    // a cutscene starts (ritual, Sipeha transition, the four endings): it plays for everyone
    "/Game/Cutscenes/CutsceneManagerBase.CutsceneManagerBase_C:BeginCutscene",
    // a pot breaks: pots are not saved (they come back after a rest), so the break itself is shared; its loot is
    // rolled where it was broken and arrives as a drop (HazardSync skips the roll during a replay)
    "/Game/Props/Pots/Pot_Base.Pot_Base_C:Break",
};
constexpr size_t kMirroredCount = sizeof(kMirrored) / sizeof(kMirrored[0]);

std::string FunctionName(const char* path) {
    const char* c = std::strrchr(path, ':');
    return c ? std::string(c + 1) : std::string(path);
}

double NowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void QueueEvent(size_t index, SDK::UObject* obj) {
    auto& s = S();
    if (s.replaying || !coop::InSession() || !obj || !ue::IsValid(obj)) return;
    // while a save loads (death, joining) the game re-runs triggers to restore them: that is not a player's action
    if (NowSec() < s.quietUntil || !ActiveSave() || !ue::LocalCharacter()) return;
    const std::string path = ue::PathName(obj);
    if (path.empty() || path.size() > 400) return;
    s.eventsOut.push_back("e" + std::to_string(index) + ":" + path);
}

// Runs an event received from another machine on our copy of the object.
void RunEvent(const std::string& key) {
    auto& s = S();
    const size_t colon = key.find(':');
    if (key.size() < 4 || key[0] != 'e' || colon == std::string::npos) return;
    const size_t index = static_cast<size_t>(std::atoi(key.substr(1, colon - 1).c_str()));
    if (index >= kMirroredCount) return;
    const std::string path = key.substr(colon + 1);
    SDK::UObject* obj = ue::FindObject(path);
    if (!obj || !ue::IsValid(obj)) {
        QD_INFO(kTag, "event {} for {}: not loaded here, skipped", FunctionName(kMirrored[index]), path);
        return;
    }
    SDK::UFunction* fn = ue::FindFunction(obj, FunctionName(kMirrored[index]));
    if (!fn) return;
    alignas(16) uint8_t parms[512] = {};   // DoTrigger has none; BeginCutscene's "Ended" delegate stays unbound
    s.replaying = true;
    s.replayed[obj] = NowSec();
    ue::CallFunction(obj, fn, parms);
    s.replaying = false;
    QD_INFO(kTag, "ran {} on {}", FunctionName(kMirrored[index]), path);
}

// ------------------------------------------------------------------ diff / send

void Flags(const Snapshot& from, const Snapshot* minus, std::vector<net::FlagValue>& out) {
    for (auto& [g, v] : from.states) {
        if (minus) {
            auto it = minus->states.find(g);
            if (it != minus->states.end() && it->second == v) continue;
        }
        out.push_back({GuidKey(g), v});
    }
    for (auto& [n, v] : from.named) {
        if (minus) {
            auto it = minus->named.find(n);
            if (it != minus->named.end() && it->second == v) continue;
        }
        out.push_back({"n" + n, v});
    }
    for (auto& p : from.visited)
        if (!minus || !minus->visited.count(p)) out.push_back({"v" + p, 1});
    for (auto& p : from.teleporters)
        if (!minus || !minus->teleporters.count(p)) out.push_back({"t" + p, 1});
    if (!from.lastCheckpoint.empty() && (!minus || minus->lastCheckpoint != from.lastCheckpoint))
        out.push_back({"c" + from.lastCheckpoint, 1});
    if (!from.savePoint.empty() && (!minus || minus->savePoint != from.savePoint))
        out.push_back({"s" + from.savePoint, 1});
}

void Remember(Snapshot& k, const std::string& name, int32_t value) {
    if (name.size() < 2) return;
    const std::string key = name.substr(1);
    switch (name[0]) {
        case 'g': { Guid g{}; if (ParseGuid(key, g)) k.states[g] = value; break; }
        case 'n': k.named[key] = value; break;
        case 'v': k.visited.insert(key); break;
        case 't': k.teleporters.insert(key); break;
        case 'c': k.lastCheckpoint = key; break;
        case 's': k.savePoint = key; break;
        case 'x': { Guid g{}; if (ParseGuid(key, g)) S().taken.insert(g); break; }
        default: break;
    }
}

template <class SendFn>
void SendChunks(const std::vector<net::FlagValue>& flags, bool full, SendFn&& send) {
    size_t i = 0;
    do {
        net::MsgWorldFlags m;
        m.full = full && i == 0;
        const size_t end = (std::min)(flags.size(), i + kFlagsPerMsg);
        m.flags.assign(flags.begin() + i, flags.begin() + end);
        send(net::Encode(m));
        i = end;
    } while (i < flags.size());
}

void RelayExcept(uint8_t exceptSlot, const std::vector<uint8_t>& bytes) {
    for (const coop::Member& m : coop::Members())
        if (m.present && !m.isLocal && m.slot != exceptSlot) coop::SendToSlot(m.slot, net::Channel::Reliable, bytes);
}

// Entries of `known` that the local save does not show (yet): received while nothing could write them, or lost when
// the game reloaded the save from disk (death). Known wins.
// onlyMissing: leave entries alone that exist locally with another value (that is a local change about to be sent).
void Reassert(SDK::UFS_SaveSlot_C* save, const Snapshot& cur, bool onlyMissing) {
    auto& s = S();
    std::vector<net::FlagValue> missing;
    Flags(s.known, &cur, missing);
    int applied = 0;
    for (auto& f : missing) {
        if (onlyMissing && f.name.size() > 1) {
            const std::string key = f.name.substr(1);
            Guid g{};
            if (f.name[0] == 'g' && ParseGuid(key, g) && cur.states.count(g)) continue;
            if (f.name[0] == 'n' && cur.named.count(key)) continue;
            if (f.name[0] == 'c' || f.name[0] == 's') continue;
        }
        if (ApplyEntry(save, f.name, f.value)) ++applied;
    }
    for (const Guid& g : s.taken) {
        bool here = cur.drops.count(g) > 0;
        if (!here)
            if (auto it = s.live.find(g); it != s.live.end() && !it->second.empty()) here = true;
        if (here && ApplyTaken(save, g)) ++applied;
    }
    if (applied) QD_INFO(kTag, "re-applied {} shared entries to the local save", applied);
}

bool SyncSessionFlag();

void OnWorldFlags(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    net::MsgWorldFlags m;
    if (!net::Decode(data, size, m)) return;
    if (!coop::IsHost() && fromSlot != 0) return;
    SyncSessionFlag();
    const bool host = coop::IsHost();
    if (m.full && !host) s.haveFull = true;
    SDK::UFS_SaveSlot_C* save = ActiveSave();
    if (save) RebuildIndex();
    int applied = 0;
    for (auto& f : m.flags) {
        if (!f.name.empty() && f.name[0] == 'e') {   // an event: run it once, never remembered
            RunEvent(f.name);
            continue;
        }
        Remember(s.known, f.name, f.value);   // even when it cannot be written now: Reassert catches up
        if (save && ApplyEntry(save, f.name, f.value)) ++applied;
        else s.pending = true;
    }
    if (save && applied) RebuildIndex();   // trims what the restores appended
    if (host) RelayExcept(fromSlot, net::Encode(m));
    QD_INFO(kTag, "{} entries from slot {}{} ({} applied)", m.flags.size(), fromSlot, m.full ? " (full state)" : "", applied);
}

// Session start / end. Called from the ticker and from the message handler: the host's full state can arrive before
// this machine's ticker has noticed the session, and must not be wiped by that notice afterwards.
bool SyncSessionFlag() {
    auto& s = S();
    const bool in = coop::InSession();
    if (in != s.inSession) {
        s.inSession = in;
        s.known = Snapshot{};
        s.haveFull = false;
        s.lastSave = nullptr;
        s.pending = false;
        s.taken.clear();
        s.dropsSeen.clear();
        s.pollIn = 0;
        if (in) QD_INFO(kTag, "session started as {}", coop::IsHost() ? "host" : "client");
    }
    return in;
}

void Tick(float dt) {
    auto& s = S();
    if (!SyncSessionFlag()) {
        s.eventsOut.clear();
        return;
    }
    if (!s.eventsOut.empty()) {
        std::vector<net::FlagValue> ev;
        for (auto& e : s.eventsOut) ev.push_back({e, 0});
        s.eventsOut.clear();
        SendChunks(ev, false, [](const std::vector<uint8_t>& b) { coop::Broadcast(net::Channel::Reliable, b); });
        QD_INFO(kTag, "sent {} event(s), first {}", ev.size(), ev.front().name);
    }
    s.pollIn -= dt;
    s.indexIn -= dt;
    if (s.pollIn > 0) return;
    s.pollIn = kPollSec;

    SDK::UFS_SaveSlot_C* save = ActiveSave();
    if (!save || !ue::LocalCharacter()) return;
    const bool host = coop::IsHost();
    SDK::UWorld* world = ue::World();
    const bool reloaded = save != s.lastSave || world != s.lastWorld;
    if (reloaded) s.quietUntil = NowSec() + 5.0;
    const bool reindexed = s.indexIn <= 0 || reloaded;
    if (reindexed) {
        s.indexIn = kIndexSec;
        RebuildIndex();
    }
    Snapshot cur = Read(save);

    if (host && s.lastSave == nullptr) {
        s.known = cur;   // the host's world is the session's starting point
    } else if (reloaded || s.pending || reindexed) {
        // new save object (death reload), entries that arrived while nothing could write them, or a periodic check
        Reassert(save, cur, !(reloaded || s.pending));
        s.pending = false;
        cur = Read(save);
    }
    s.lastSave = save;
    s.lastWorld = world;
    if (!host && !s.haveFull) {   // a client says nothing before it has the host's world
        s.dropsSeen = cur.drops;
        return;
    }

    std::vector<net::FlagValue> changed;
    Flags(cur, &s.known, changed);
    // a drop whose save entry went away while the save stayed the same object was picked up here
    if (!reloaded)
        for (const Guid& g : s.dropsSeen)
            if (!cur.drops.count(g) && !s.taken.count(g)) {
                s.taken.insert(g);
                changed.push_back({GuidKey(g, 'x'), 0});
            }
    s.dropsSeen = cur.drops;
    if (changed.empty()) return;
    // An entry that differs from `known` right after a reload is the reload, not a player's action: known wins and
    // Reassert has just rewritten it. What still differs now is local progress.
    for (auto& f : changed) Remember(s.known, f.name, f.value);
    SendChunks(changed, false, [](const std::vector<uint8_t>& b) { coop::Broadcast(net::Channel::Reliable, b); });
    QD_INFO(kTag, "sent {} changed entries", changed.size());
}

void OnMember(const coop::Member& m, bool joined) {
    if (m.isLocal || !joined || !coop::IsHost()) return;
    auto& s = S();
    SyncSessionFlag();
    if (SDK::UFS_SaveSlot_C* save = ActiveSave(); save && s.lastSave == nullptr) {
        s.known = Read(save);
        s.lastSave = save;
        s.lastWorld = ue::World();
    }
    std::vector<net::FlagValue> all;
    Flags(s.known, nullptr, all);
    for (const Guid& g : s.taken) all.push_back({GuidKey(g, 'x'), 0});
    const uint8_t slot = m.slot;
    SendChunks(all, true, [slot](const std::vector<uint8_t>& b) { coop::SendToSlot(slot, net::Channel::Reliable, b); });
    QD_INFO(kTag, "sent the full world state ({} entries) to slot {}", all.size(), slot);
}

}  // namespace

bool WorldSyncReplaying() { return S().replaying; }

bool WorldSyncReplayedRecently(const void* obj, double seconds) {
    auto& s = S();
    const double now = NowSec();
    std::erase_if(s.replayed, [&](const auto& kv) { return now - kv.second > 60.0; });
    auto it = s.replayed.find(obj);
    return it != s.replayed.end() && now - it->second <= seconds;
}

void WorldSyncInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    coop::On(net::MsgType::WorldFlags, &OnWorldFlags);
    coop::AddMemberListener(&OnMember);
    for (size_t i = 0; i < kMirroredCount; ++i)
        hooks::AddPre(kMirrored[i], [i](hooks::CallInfo& ci) {
            QueueEvent(i, ci.object);
            return false;
        });
    s.ticker = gamethread::AddTicker("worldsync", [](float dt) { Tick(dt); });
    QD_INFO(kTag, "initialized");
}

void WorldSyncShutdown() {
    auto& s = S();
    if (!s.inited) return;
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    s.inited = false;
}

}  // namespace qd::sync
