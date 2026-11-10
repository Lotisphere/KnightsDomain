#include "qdcoop/core/Hooks.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Guard.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/core/Util.h"

#include <Windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"

// -----------------------------------------------------------------------------------------------------------------
// Detours (all installed with MinHook from the core init thread):
//   1. UObject::ProcessEvent            vtable[Offsets::ProcessEventIdx] of a non-actor UObject (AActor overrides it
//                                       and forwards to Super, so the UObject version catches actors too).
//                                       Pumps the game-thread queue; dispatches FUNC_Native functions only.
//   2. UObject::ProcessInternal         UFunction::Func of any non-native UFunction (no signature scan).
//                                       Script functions entered through ProcessEvent (C++ -> BP, BIE events).
//   3. ProcessLocalScriptFunction       script functions called from other script code (BP -> BP).
//                                       In UE >= 4.22 the VM's CallFunction no longer goes through UFunction::Func,
//                                       so ProcessInternal alone never sees BP -> BP calls (e.g. Interact called by
//                                       the player BP). Its address is a build-specific RVA
//                                       and is only used when ProcessInternal is found at its known
//                                       RVA too, i.e. the exe is the same build. Disable with -qdcoop-noplsf.
//      ProcessInternal calls ProcessLocalScriptFunction for its own frame; that inner call is de-duplicated so a
//      script function is dispatched exactly once whichever way it was entered.
//
// Dispatch key: the called UFunction and every function up its SuperStruct chain (a Blueprint override's
// SuperStruct is the function it overrides), so a hook on "/Script/...:GetTargetPlayer" also fires for the
// EnemyBase_C override. Hooks registered on both a parent and an override both fire.
// -----------------------------------------------------------------------------------------------------------------

namespace qd::hooks {
namespace {
constexpr char kTag[] = "hooks";

// FFrame (UE 4.27): FOutputDevice (vtable + 2 bools, padded to 0x10), then:
constexpr size_t kFrameNode = 0x10;    // UFunction*
constexpr size_t kFrameObject = 0x18;  // UObject*
constexpr size_t kFrameLocals = 0x28;  // uint8*
// UFunction (UE 4.27): FunctionFlags 0xB0, NumParms 0xB4, ParmsSize 0xB6, ReturnValueOffset 0xB8, ..., Func 0xD8
constexpr size_t   kFuncReturnValueOffset = 0xB8;
constexpr uint16_t kNoReturnValue = 0xFFFF;

// QueensDomain-Win64-Shipping.exe, 81,374,720 bytes.
constexpr uintptr_t kKnownProcessInternalRva = 0x011A7E40;
constexpr uintptr_t kKnownProcessLocalScriptFunctionRva = 0x011A7F50;

constexpr int      kMaxChainDepth = 16;
constexpr int      kMaxLists = 8;
constexpr uint64_t kResolveIntervalMs = 500;
constexpr uint64_t kUnresolvedWarnMs = 30000;

struct FFrame;  // opaque; accessed by offset only
using ProcessEventFn = void (*)(SDK::UObject*, SDK::UFunction*, void*);
using ScriptFn = void (*)(SDK::UObject* Context, FFrame& Stack, void* Result);

struct Entry {
    HookId                id = 0;
    std::string           path;
    bool                  isPre = false;
    PreCallback           pre;
    PostCallback          post;
    std::atomic<bool>     alive{true};
    uint64_t              registeredMs = 0;
    bool                  warnedUnresolved = false;
};
using EntryPtr = std::shared_ptr<Entry>;

struct HookList {
    std::vector<EntryPtr> pre;
    std::vector<EntryPtr> post;
};

struct ResolvedKey {
    int32_t   index = -1;
    SDK::FName name;
};

struct TraceRequest {
    std::string className;
    float       seconds = 0.f;
};

struct Trace {
    std::string                         className;
    SDK::FName                          name;
    bool                                nameReady = false;
    uint64_t                            endMs = 0;
    std::unordered_set<SDK::UFunction*> seen;
};

// ---- registration side: any thread ----
struct Registry {
    std::mutex                              mutex;
    std::vector<EntryPtr>                   pendingAdds;
    std::unordered_map<HookId, EntryPtr>    byId;
    std::vector<TraceRequest>               pendingTraces;
    std::atomic<bool>                       dirty{false};
    std::atomic<HookId>                     nextId{1};
};
Registry& Reg() {
    static Registry r;  // function-local: safe to use from other TUs' static initializers
    return r;
}

// ---- game-thread-only state ----
std::unordered_map<SDK::UFunction*, HookList>    g_table;
uint32_t g_tableGen = 1;   // bumped on every g_table change (see the no-hook cache in the dispatch section)
std::unordered_map<SDK::UFunction*, ResolvedKey> g_keys;
std::vector<EntryPtr>                            g_unresolved;
std::vector<Trace>                               g_traces;
int      g_depth = 0;          // > 0 while a dispatch (callbacks or the original call) is on the stack
bool     g_inMaintain = false;  // re-entrancy guard for table maintenance / resolution
bool     g_needCompact = false;
uint64_t g_lastResolveMs = 0;

// ProcessInternal -> ProcessLocalScriptFunction de-duplication (game thread only)
struct PendingScript {
    void*           frame = nullptr;
    SDK::UFunction* fn = nullptr;
    SDK::UObject*   ctx = nullptr;
};
PendingScript g_piPending;

// ---- detour state ----
ProcessEventFn    o_ProcessEvent = nullptr;
ScriptFn          o_ProcessInternal = nullptr;
ScriptFn          o_ProcessLocalScriptFunction = nullptr;
void*             g_peTarget = nullptr;
void*             g_piTarget = nullptr;
void*             g_plsfTarget = nullptr;
std::atomic<bool> g_installed{false};
gamethread::TickerId g_maintTicker = 0;

enum class FrameCheck : int { Unknown, Ok, Broken };
FrameCheck g_piCheck = FrameCheck::Unknown;    // game thread only
FrameCheck g_plsfCheck = FrameCheck::Unknown;  // game thread only

std::atomic<bool> g_loggedGameThreadPE{false};
std::atomic<bool> g_loggedOtherThreadPE{false};

// ------------------------------------------------------------------------------------------------ raw accessors
inline SDK::UFunction* FrameNode(FFrame& s) {
    return *reinterpret_cast<SDK::UFunction**>(reinterpret_cast<uint8_t*>(&s) + kFrameNode);
}
inline SDK::UObject* FrameObject(FFrame& s) {
    return *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uint8_t*>(&s) + kFrameObject);
}
inline void* FrameLocals(FFrame& s) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(&s) + kFrameLocals);
}
inline uint16_t ReturnValueOffset(SDK::UFunction* fn) {
    return *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(fn) + kFuncReturnValueOffset);
}
inline bool IsNative(SDK::UFunction* fn) {
    return (static_cast<uint32_t>(fn->FunctionFlags) & static_cast<uint32_t>(SDK::EFunctionFlags::Native)) != 0;
}
inline bool HasCast(const SDK::UObject* o, SDK::EClassCastFlags f) {
    return o && o->Class && (static_cast<uint64_t>(o->Class->CastFlags) & static_cast<uint64_t>(f)) != 0;
}

// SEH-protected frame check (no C++ objects with destructors here).
bool VerifyFrameRaw(FFrame* stack, SDK::UFunction** outNode, SDK::UObject** outObject) {
    __try {
        uint8_t* base = reinterpret_cast<uint8_t*>(stack);
        SDK::UFunction* node = *reinterpret_cast<SDK::UFunction**>(base + kFrameNode);
        SDK::UObject* object = *reinterpret_cast<SDK::UObject**>(base + kFrameObject);
        *outNode = node;
        *outObject = object;
        if (!node || !node->Class) return false;
        return (static_cast<uint64_t>(node->Class->CastFlags) & static_cast<uint64_t>(SDK::EClassCastFlags::Function)) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

FrameCheck VerifyFrame(const char* which, SDK::UObject* ctx, FFrame& stack) {
    SDK::UFunction* node = nullptr;
    SDK::UObject* object = nullptr;
    if (!VerifyFrameRaw(&stack, &node, &object)) {
        QD_ERROR(kTag, "{}: FFrame check FAILED (Node@+0x{:X}={} is not a UFunction). Script dispatch disabled; detour is passthrough.",
                 which, kFrameNode, static_cast<void*>(node));
        return FrameCheck::Broken;
    }
    QD_INFO(kTag, "{}: FFrame layout verified (Node={}, Object {} Context)", which, ue::PathName(node),
            object == ctx ? "==" : "!=");
    return FrameCheck::Ok;
}

// -------------------------------------------------------------------------------------------------- table upkeep
void CompactNow() {
    for (auto it = g_table.begin(); it != g_table.end();) {
        auto dead = [](const EntryPtr& e) { return !e->alive.load(std::memory_order_relaxed); };
        std::erase_if(it->second.pre, dead);
        std::erase_if(it->second.post, dead);
        if (it->second.pre.empty() && it->second.post.empty()) {
            g_keys.erase(it->first);
            it = g_table.erase(it);
            ++g_tableGen;
        } else {
            ++it;
        }
    }
    std::erase_if(g_unresolved, [](const EntryPtr& e) { return !e->alive.load(std::memory_order_relaxed); });
}

// Cheap, no engine calls: merge pending registrations / removals. Only at dispatch depth 0.
void MaintainLight() {
    Registry& r = Reg();
    if (!g_needCompact && !r.dirty.load(std::memory_order_acquire)) return;
    g_inMaintain = true;
    if (r.dirty.exchange(false, std::memory_order_acq_rel)) {
        std::vector<EntryPtr> adds;
        std::vector<TraceRequest> traces;
        {
            std::lock_guard lock(r.mutex);
            adds.swap(r.pendingAdds);
            traces.swap(r.pendingTraces);
        }
        for (auto& e : adds) {
            if (e->alive.load(std::memory_order_relaxed)) g_unresolved.push_back(std::move(e));
        }
        for (auto& t : traces) {
            Trace tr;
            tr.className = std::move(t.className);
            tr.endMs = util::NowMs() + static_cast<uint64_t>(t.seconds * 1000.f);
            g_traces.push_back(std::move(tr));
        }
        if (!adds.empty()) g_lastResolveMs = 0;  // resolve on the next maintenance tick
        g_needCompact = true;                   // removals also set `dirty`
    }
    if (g_needCompact) {
        CompactNow();
        g_needCompact = false;
    }
    g_inMaintain = false;
}

bool StillValid(SDK::UFunction* fn, const ResolvedKey& key) {
    return ue::IsValid(fn) && fn->Index == key.index && fn->Name == key.name;
}

void Attach(const EntryPtr& e, SDK::UFunction* fn) {
    HookList& list = g_table[fn];
    ++g_tableGen;
    (e->isPre ? list.pre : list.post).push_back(e);
    g_keys.try_emplace(fn, ResolvedKey{fn->Index, fn->Name});
}

// Engine calls allowed (FindObject/MakeName go through ProcessEvent). Runs from a game-thread ticker.
void ResolveTick() {
    if (g_depth != 0 || g_inMaintain) return;  // never mutate the table under an active dispatch
    MaintainLight();
    const uint64_t now = util::NowMs();
    if (g_lastResolveMs != 0 && now - g_lastResolveMs < kResolveIntervalMs) return;
    g_lastResolveMs = now;
    g_inMaintain = true;

    // 1. Functions that went away (BP class unloaded) -> back to unresolved.
    for (auto it = g_keys.begin(); it != g_keys.end();) {
        if (StillValid(it->first, it->second)) {
            ++it;
            continue;
        }
        auto tableIt = g_table.find(it->first);
        if (tableIt != g_table.end()) {
            for (auto& e : tableIt->second.pre) g_unresolved.push_back(e);
            for (auto& e : tableIt->second.post) g_unresolved.push_back(e);
            g_table.erase(tableIt);
            ++g_tableGen;
        }
        QD_INFO(kTag, "hooked function at {} is gone (unloaded?); will re-resolve", static_cast<void*>(it->first));
        it = g_keys.erase(it);
    }

    // 2. Resolve pending paths (one FindObject per distinct path).
    if (!g_unresolved.empty()) {
        std::vector<EntryPtr> work;
        work.swap(g_unresolved);
        std::unordered_map<std::string, SDK::UFunction*> cache;
        std::vector<EntryPtr> still;
        for (auto& e : work) {
            if (!e->alive.load(std::memory_order_relaxed)) continue;
            SDK::UFunction* fn = nullptr;
            auto c = cache.find(e->path);
            if (c != cache.end()) {
                fn = c->second;
            } else {
                SDK::UObject* obj = ue::FindObject(e->path);
                if (obj && !HasCast(obj, SDK::EClassCastFlags::Function)) {
                    QD_WARN(kTag, "'{}' resolved to a non-function ({}); hook #{} ignored", e->path,
                            ue::ClassName(obj), e->id);
                    e->alive.store(false, std::memory_order_relaxed);
                    continue;
                }
                fn = static_cast<SDK::UFunction*>(obj);
                cache.emplace(e->path, fn);
            }
            if (fn) {
                Attach(e, fn);
                QD_INFO(kTag, "{} hook #{} resolved: {} ({})", e->isPre ? "pre" : "post", e->id, e->path,
                        IsNative(fn) ? "native" : "script");
            } else {
                if (!e->warnedUnresolved && now - e->registeredMs > kUnresolvedWarnMs) {
                    e->warnedUnresolved = true;
                    QD_WARN(kTag, "hook #{} still unresolved after {} s: {} (class not loaded yet, or wrong path)",
                            e->id, kUnresolvedWarnMs / 1000, e->path);
                }
                still.push_back(std::move(e));
            }
        }
        // Entries that arrived during resolution were merged into g_unresolved; keep them too.
        for (auto& e : g_unresolved) still.push_back(std::move(e));
        g_unresolved.swap(still);
    }

    // 3. Traces: create class FNames, expire finished ones.
    for (auto it = g_traces.begin(); it != g_traces.end();) {
        if (!it->nameReady) {
            it->name = ue::MakeName(it->className);
            it->nameReady = true;
            QD_INFO("trace", "tracing functions called on '{}' objects", it->className);
        }
        if (now >= it->endMs) {
            QD_INFO("trace", "trace of '{}' ended: {} distinct functions", it->className, it->seen.size());
            it = g_traces.erase(it);
        } else {
            ++it;
        }
    }
    g_inMaintain = false;
}

// --------------------------------------------------------------------------------------------------- dispatch
// every Blueprint function call in the game passes through here, and almost none of them is hooked. A small
// direct-mapped "this function has no hooks anywhere up its chain" cache turns the common case into one compare
// instead of a hash lookup per SuperStruct level. g_tableGen changes whenever g_table does, which empties the cache
// (a UFunction freed and reallocated at the same address can only gain hooks through a table change).
struct NoHookSlot {
    const SDK::UFunction* fn = nullptr;
    uint32_t              gen = 0;
};
constexpr size_t kNoHookSlots = 8192;   // power of two
NoHookSlot g_noHook[kNoHookSlots];

inline size_t NoHookIndex(const SDK::UFunction* fn) {
    return (reinterpret_cast<uintptr_t>(fn) >> 4) & (kNoHookSlots - 1);
}

int CollectListsSlow(SDK::UFunction* fn, HookList** out);

int CollectLists(SDK::UFunction* fn, HookList** out) {
    NoHookSlot& slot = g_noHook[NoHookIndex(fn)];
    if (slot.fn == fn && slot.gen == g_tableGen) return 0;
    int n = CollectListsSlow(fn, out);
    if (n == 0) {
        slot.fn = fn;
        slot.gen = g_tableGen;
    }
    return n;
}

int CollectListsSlow(SDK::UFunction* fn, HookList** out) {
    int n = 0;
    int depth = 0;
    for (SDK::UStruct* s = fn; s && n < kMaxLists && depth < kMaxChainDepth; s = s->SuperStruct, ++depth) {
        auto it = g_table.find(static_cast<SDK::UFunction*>(s));
        if (it != g_table.end()) out[n++] = &it->second;
    }
    return n;
}

void TraceCall(SDK::UObject* obj, SDK::UFunction* fn) {
    if (!obj || !fn || !obj->Class) return;
    for (Trace& t : g_traces) {
        if (!t.nameReady) continue;
        bool match = false;
        int depth = 0;
        for (const SDK::UStruct* c = obj->Class; c && depth < 32; c = c->SuperStruct, ++depth) {
            if (c->Name == t.name) {
                match = true;
                break;
            }
        }
        if (match && t.seen.insert(fn).second) {
            QD_INFO("trace", "[{}] {} ({})", t.className, ue::PathName(fn), IsNative(fn) ? "native" : "script");
        }
    }
}

void DisableEntry(Entry* e) {
    e->alive.store(false, std::memory_order_relaxed);
    g_needCompact = true;
    QD_ERROR(kTag, "hook #{} ({}) disabled after a structured exception", e->id, e->path);
    std::lock_guard lock(Reg().mutex);
    Reg().byId.erase(e->id);
}

template <class CallOriginal>
void Dispatch(SDK::UObject* obj, SDK::UFunction* fn, void* params, void* result, HookList** lists, int n,
              CallOriginal&& callOriginal) {
    CallInfo ci{obj, fn, params, result};
    bool skip = false;
    ++g_depth;
    // Lists are never mutated while g_depth > 0, so indices stay valid; callbacks may only flip `alive`.
    for (int i = 0; i < n; ++i) {
        for (size_t k = 0; k < lists[i]->pre.size(); ++k) {
            Entry* e = lists[i]->pre[k].get();
            if (!e->alive.load(std::memory_order_relaxed)) continue;
            bool wantSkip = false;
            const auto r = detail::GuardedCall(kTag, e->path.c_str(), [&] { wantSkip = e->pre(ci); });
            if (r == detail::GuardResult::SehException) DisableEntry(e);
            else if (r == detail::GuardResult::Ok && wantSkip) skip = true;
        }
    }
    if (!skip) callOriginal();
    // Post callbacks run even when a pre callback skipped the original (they see whatever it wrote to result).
    for (int i = 0; i < n; ++i) {
        for (size_t k = 0; k < lists[i]->post.size(); ++k) {
            Entry* e = lists[i]->post[k].get();
            if (!e->alive.load(std::memory_order_relaxed)) continue;
            const auto r = detail::GuardedCall(kTag, e->path.c_str(), [&] { e->post(ci); });
            if (r == detail::GuardResult::SehException) DisableEntry(e);
        }
    }
    --g_depth;
}

// ---------------------------------------------------------------------------------------------------- detours
void Detour_ProcessEvent(SDK::UObject* obj, SDK::UFunction* fn, void* parms) {
    if (!core::IsInGameThread()) {
        if (!g_loggedOtherThreadPE.load(std::memory_order_relaxed) && !g_loggedOtherThreadPE.exchange(true))
            QD_INFO(kTag, "ProcessEvent also seen on non-game thread {} (passthrough there)", GetCurrentThreadId());
        o_ProcessEvent(obj, fn, parms);
        return;
    }
    if (!g_loggedGameThreadPE.load(std::memory_order_relaxed) && !g_loggedGameThreadPE.exchange(true))
        QD_INFO(kTag, "ProcessEvent detour live on game thread {}", GetCurrentThreadId());

    gamethread::Pump();
    if (g_depth == 0 && !g_inMaintain) MaintainLight();
    if (fn && !g_traces.empty()) TraceCall(obj, fn);

    if (!fn || g_table.empty() || !IsNative(fn)) {
        o_ProcessEvent(obj, fn, parms);
        return;
    }
    HookList* lists[kMaxLists];
    const int n = CollectLists(fn, lists);
    if (n == 0) {
        o_ProcessEvent(obj, fn, parms);
        return;
    }
    void* result = nullptr;
    const uint16_t off = ReturnValueOffset(fn);
    if (parms && off != kNoReturnValue) result = static_cast<uint8_t*>(parms) + off;
    Dispatch(obj, fn, parms, result, lists, n, [&] { o_ProcessEvent(obj, fn, parms); });
}

template <class CallOriginal>
void ScriptDispatch(SDK::UObject* ctx, FFrame& stack, void* result, CallOriginal&& callOriginal) {
    if (g_depth == 0 && !g_inMaintain) MaintainLight();
    SDK::UFunction* fn = FrameNode(stack);
    SDK::UObject* obj = FrameObject(stack);
    if (!obj) obj = ctx;
    if (fn && !g_traces.empty()) TraceCall(obj, fn);
    if (!fn || g_table.empty()) {
        callOriginal();
        return;
    }
    HookList* lists[kMaxLists];
    const int n = CollectLists(fn, lists);
    if (n == 0) {
        callOriginal();
        return;
    }
    Dispatch(obj, fn, FrameLocals(stack), result, lists, n, callOriginal);
}

void Detour_ProcessInternal(SDK::UObject* ctx, FFrame& stack, void* result) {
    if (g_piCheck == FrameCheck::Broken || !core::IsInGameThread()) {
        o_ProcessInternal(ctx, stack, result);
        return;
    }
    if (g_piCheck == FrameCheck::Unknown) {
        g_piCheck = VerifyFrame("ProcessInternal", ctx, stack);
        if (g_piCheck == FrameCheck::Broken) {
            o_ProcessInternal(ctx, stack, result);
            return;
        }
    }
    auto callOriginal = [&] {
        // ProcessInternal runs the bytecode through ProcessLocalScriptFunction on this same frame:
        // tell the PLSF detour not to dispatch that inner call again.
        const PendingScript saved = g_piPending;
        g_piPending = PendingScript{&stack, FrameNode(stack), ctx};
        o_ProcessInternal(ctx, stack, result);
        g_piPending = saved;
    };
    ScriptDispatch(ctx, stack, result, callOriginal);
}

void Detour_ProcessLocalScriptFunction(SDK::UObject* ctx, FFrame& stack, void* result) {
    if (g_plsfCheck == FrameCheck::Broken || !core::IsInGameThread()) {
        o_ProcessLocalScriptFunction(ctx, stack, result);
        return;
    }
    // Inner call made by ProcessInternal for the frame it already dispatched: consume once, pass through.
    if (g_piPending.fn && (g_piPending.frame == &stack || (g_piPending.fn == FrameNode(stack) && g_piPending.ctx == ctx))) {
        g_piPending = PendingScript{};
        o_ProcessLocalScriptFunction(ctx, stack, result);
        return;
    }
    if (g_plsfCheck == FrameCheck::Unknown) {
        g_plsfCheck = VerifyFrame("ProcessLocalScriptFunction", ctx, stack);
        if (g_plsfCheck == FrameCheck::Broken) {
            o_ProcessLocalScriptFunction(ctx, stack, result);
            return;
        }
    }
    ScriptDispatch(ctx, stack, result, [&] { o_ProcessLocalScriptFunction(ctx, stack, result); });
}

// ------------------------------------------------------------------------------------------- address discovery
uintptr_t ImageBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool InMainImage(const void* p) {
    const uintptr_t base = ImageBase();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= base && a < base + nt->OptionalHeader.SizeOfImage;
}

// SEH-protected (no C++ objects). Returns vtable[ProcessEventIdx] of the first non-actor UObject.
void* FindProcessEventRaw() {
    __try {
        SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
        const int32_t num = arr->NumElements < 4096 ? arr->NumElements : 4096;
        for (int32_t i = 0; i < num; ++i) {
            SDK::UObject* obj = arr->GetByIndex(i);
            if (!obj || !obj->VTable || !obj->Class) continue;
            if (static_cast<uint64_t>(obj->Class->CastFlags) & static_cast<uint64_t>(SDK::EClassCastFlags::Actor)) continue;
            void** vtable = *reinterpret_cast<void***>(obj);
            return vtable[SDK::Offsets::ProcessEventIdx];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// SEH-protected (no C++ objects). Collects UFunction::Func of up to `max` non-native UFunctions.
int SampleScriptFuncsRaw(void** out, int max) {
    int n = 0;
    __try {
        SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
        const int32_t num = arr->NumElements;
        for (int32_t i = 0; i < num && n < max; ++i) {
            SDK::UObject* obj = arr->GetByIndex(i);
            if (!obj || !obj->Class) continue;
            if (!(static_cast<uint64_t>(obj->Class->CastFlags) & static_cast<uint64_t>(SDK::EClassCastFlags::Function))) continue;
            auto* fn = static_cast<SDK::UFunction*>(obj);
            if (static_cast<uint32_t>(fn->FunctionFlags) & static_cast<uint32_t>(SDK::EFunctionFlags::Native)) continue;
            if (!fn->ExecFunction) continue;
            out[n++] = reinterpret_cast<void*>(fn->ExecFunction);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void* FindProcessEvent() {
    void* fromVtable = FindProcessEventRaw();
    void* fromOffsets = reinterpret_cast<void*>(ImageBase() + SDK::Offsets::ProcessEvent);
    if (!fromVtable) {
        QD_ERROR(kTag, "could not read ProcessEvent from a UObject vtable");
        return nullptr;
    }
    if (fromVtable != fromOffsets) {
        QD_ERROR(kTag, "ProcessEvent mismatch: vtable {} vs SDK Offsets {} — SDK may be stale for this exe; using vtable",
                 fromVtable, fromOffsets);
    } else {
        QD_INFO(kTag, "ProcessEvent at {} (matches SDK Offsets)", fromVtable);
    }
    return InMainImage(fromVtable) ? fromVtable : nullptr;
}

void* FindProcessInternal() {
    void* samples[32] = {};
    const int n = SampleScriptFuncsRaw(samples, 32);
    if (n == 0) {
        QD_ERROR(kTag, "no non-native UFunction with a Func pointer found");
        return nullptr;
    }
    // Majority vote: every non-native UFunction is bound to UObject::ProcessInternal.
    void* best = nullptr;
    int bestCount = 0;
    for (int i = 0; i < n; ++i) {
        int c = 0;
        for (int k = 0; k < n; ++k) c += samples[k] == samples[i];
        if (c > bestCount) {
            bestCount = c;
            best = samples[i];
        }
    }
    QD_INFO(kTag, "ProcessInternal at {} ({}/{} sampled script functions agree)", best, bestCount, n);
    if (bestCount * 2 <= n) {
        QD_ERROR(kTag, "ProcessInternal vote is not a clear majority; refusing to hook");
        return nullptr;
    }
    return InMainImage(best) ? best : nullptr;
}

bool CreateHook(void* target, void* detour, void** original, const char* name) {
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        QD_ERROR(kTag, "MH_CreateHook({}) failed: {}", name, MH_StatusToString(st));
        return false;
    }
    return true;
}
}  // namespace

bool ScriptHooksComplete() { return g_plsfTarget != nullptr; }

bool Init() {
    if (g_installed.load()) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        QD_ERROR(kTag, "MH_Initialize failed: {}", MH_StatusToString(init));
        return false;
    }

    g_peTarget = FindProcessEvent();
    g_piTarget = FindProcessInternal();
    if (!g_peTarget || !g_piTarget) return false;

    if (!CreateHook(g_peTarget, reinterpret_cast<void*>(&Detour_ProcessEvent),
                    reinterpret_cast<void**>(&o_ProcessEvent), "ProcessEvent"))
        return false;
    if (!CreateHook(g_piTarget, reinterpret_cast<void*>(&Detour_ProcessInternal),
                    reinterpret_cast<void**>(&o_ProcessInternal), "ProcessInternal")) {
        MH_RemoveHook(g_peTarget);
        return false;
    }

    const uintptr_t base = ImageBase();
    if (util::CommandLineHasFlag(L"-qdcoop-noplsf")) {
        QD_WARN(kTag, "-qdcoop-noplsf: ProcessLocalScriptFunction not hooked; BP->BP calls will not dispatch");
    } else if (reinterpret_cast<uintptr_t>(g_piTarget) == base + kKnownProcessInternalRva) {
        g_plsfTarget = reinterpret_cast<void*>(base + kKnownProcessLocalScriptFunctionRva);
        if (CreateHook(g_plsfTarget, reinterpret_cast<void*>(&Detour_ProcessLocalScriptFunction),
                       reinterpret_cast<void**>(&o_ProcessLocalScriptFunction), "ProcessLocalScriptFunction")) {
            QD_INFO(kTag, "ProcessLocalScriptFunction at {} (known build RVA 0x{:X})", g_plsfTarget,
                    kKnownProcessLocalScriptFunctionRva);
        } else {
            g_plsfTarget = nullptr;
        }
    } else {
        QD_WARN(kTag,
                "ProcessInternal RVA 0x{:X} != known 0x{:X}: unknown exe build, ProcessLocalScriptFunction NOT hooked. "
                "Hooks on Blueprint functions will only fire when entered via ProcessEvent (not BP->BP calls).",
                reinterpret_cast<uintptr_t>(g_piTarget) - base, kKnownProcessInternalRva);
    }

    const MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    if (en != MH_OK) {
        QD_ERROR(kTag, "MH_EnableHook failed: {}", MH_StatusToString(en));
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(g_peTarget);
        MH_RemoveHook(g_piTarget);
        if (g_plsfTarget) MH_RemoveHook(g_plsfTarget);
        g_plsfTarget = nullptr;
        return false;
    }
    g_installed.store(true);
    g_maintTicker = gamethread::AddTicker("qd.hooks.maintain", [](float) { ResolveTick(); });
    QD_INFO(kTag, "hooks installed (ProcessEvent, ProcessInternal{})", g_plsfTarget ? ", ProcessLocalScriptFunction" : "");
    return true;
}

void Shutdown() {
    if (!g_installed.exchange(false)) return;
    if (g_maintTicker) gamethread::RemoveTicker(g_maintTicker);
    g_maintTicker = 0;
    // Disable only: trampolines may still be on some thread's stack, so MinHook is not uninitialized.
    MH_DisableHook(MH_ALL_HOOKS);
    QD_INFO(kTag, "hooks disabled");
}

namespace {
HookId Add(const char* functionPath, bool isPre, PreCallback pre, PostCallback post) {
    if (!functionPath || !*functionPath) {
        QD_WARN(kTag, "Add{}: empty function path ignored", isPre ? "Pre" : "Post");
        return 0;
    }
    Registry& r = Reg();
    auto e = std::make_shared<Entry>();
    e->id = r.nextId.fetch_add(1);
    e->path = functionPath;
    e->isPre = isPre;
    e->pre = std::move(pre);
    e->post = std::move(post);
    e->registeredMs = util::NowMs();
    {
        std::lock_guard lock(r.mutex);
        r.byId.emplace(e->id, e);
        r.pendingAdds.push_back(e);
    }
    r.dirty.store(true, std::memory_order_release);
    QD_DEBUG(kTag, "{} hook #{} registered for {}", isPre ? "pre" : "post", e->id, e->path);
    return e->id;
}
}  // namespace

HookId AddPre(const char* functionPath, PreCallback cb) {
    if (!cb) return 0;
    return Add(functionPath, true, std::move(cb), nullptr);
}

HookId AddPost(const char* functionPath, PostCallback cb) {
    if (!cb) return 0;
    return Add(functionPath, false, nullptr, std::move(cb));
}

void Remove(HookId id) {
    if (id == 0) return;
    Registry& r = Reg();
    {
        std::lock_guard lock(r.mutex);
        auto it = r.byId.find(id);
        if (it == r.byId.end()) return;
        it->second->alive.store(false, std::memory_order_relaxed);
        r.byId.erase(it);
    }
    r.dirty.store(true, std::memory_order_release);
}

void TraceClass(const char* className, float seconds) {
    if (!className || !*className || seconds <= 0.f) return;
    Registry& r = Reg();
    {
        std::lock_guard lock(r.mutex);
        r.pendingTraces.push_back(TraceRequest{className, seconds});
    }
    r.dirty.store(true, std::memory_order_release);
}

}  // namespace qd::hooks
