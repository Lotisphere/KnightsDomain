// QD Coop — UFunction hook contract.
//
// Detours, installed with MinHook once the engine is ready:
//   1. UObject::ProcessEvent  (vtable index Offsets::ProcessEventIdx of any UObject)
//      -> dispatches hooks ONLY for functions with FUNC_Native (script functions are dispatched by #2, avoid double firing)
//      -> also calls gamethread::Pump() when on the game thread
//   2. UObject::ProcessInternal (address = UFunction::ExecFunction of any non-native UFunction)
//      -> dispatches hooks for script (Blueprint) functions entered through ProcessEvent (C++ -> BP, BIE events)
//      -> FFrame (UE 4.27): +0x10 UFunction* Node, +0x18 UObject* Object, +0x20 uint8* Code, +0x28 uint8* Locals
//         Verify at runtime: Node must be a UFunction (IsA Function); log and disable dispatch if not.
//   3. ProcessLocalScriptFunction: in UE >= 4.22 BP -> BP calls skip UFunction::Func, so #2 never
//      sees them. Hooked at a build-specific RVA only when #2 sits at its known RVA (same exe). Same FFrame layout.
//      The inner PLSF call that ProcessInternal makes for its own frame is de-duplicated (one dispatch per call).
//   Dispatch matches the called UFunction AND every function up its SuperStruct chain (a BP override's SuperStruct
//   is the function it overrides), so hooking "/Script/...:GetTargetPlayer" also fires for BP overrides.
//   Limitation: native functions called directly by the BP VM (BP -> C++ via CallFunction) bypass ProcessEvent and
//   are NOT dispatched; only native calls that go through ProcessEvent are.
#pragma once
#include <cstdint>
#include <functional>

namespace SDK { class UObject; class UFunction; }

namespace qd::hooks {

struct CallInfo {
    SDK::UObject*   object = nullptr;
    SDK::UFunction* function = nullptr;
    void*           params = nullptr;   // ProcessEvent: Parms; ProcessInternal: Stack.Locals
    void*           result = nullptr;   // ProcessInternal: RESULT_PARAM (may be null); ProcessEvent: Parms + ReturnValueOffset or null
};

// Return true to SKIP the original function (only allowed for functions without latent/ubergraph behaviour).
// When skipping a function with a return value, write *result yourself. Post callbacks run even when skipped.
// Callbacks run on the game thread; a structured exception (AV) inside one permanently disables that hook.
using PreCallback  = std::function<bool(CallInfo&)>;
using PostCallback = std::function<void(CallInfo&)>;
using HookId = uint32_t;

bool Init();      // called by core once the engine is ready
void Shutdown();

// functionPath: full UFunction path name, e.g. "/Script/QueensDomain.FS_EnemyCharacterBase:GetTargetPlayer"
// or "/Game/Blueprints/ItemPickup_Base.ItemPickup_Base_C:Interact". Resolved lazily (BP classes may load later;
// retried at most every 500 ms, re-resolved if the class unloads). Thread-safe; takes effect on the game thread.
// A hook on a parent-class function also fires for overrides (SuperStruct chain); a hook registered on both the
// parent and an override fires both.
HookId AddPre(const char* functionPath, PreCallback cb);
HookId AddPost(const char* functionPath, PostCallback cb);
void   Remove(HookId id);

// False when the exe is a build this mod does not know: Blueprint -> Blueprint calls are then not dispatched, so several
// sync features silently miss events. The UI warns the player.
bool ScriptHooksComplete();

// Debug: log every distinct function name seen for objects of this class name for N seconds (debugging aid).
void TraceClass(const char* className, float seconds);

}  // namespace qd::hooks
