// QD Coop — game-state queries.
// Include order: SDK headers FIRST, then qdcoop headers. core/Unreal.h forward-declares `struct FName` while the SDK
// defines `class FName`; MSVC mangles with the first-seen class-key, so the SDK must be seen first to match the
// SDK *_functions.cpp translation units.
#include "SDK/Engine_classes.hpp"
#include "SDK/FS_GameInstance_parameters.hpp"

#include "qdcoop/sync/GameQueries.h"

#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"

namespace qd::sync {

int QueryActiveSaveSlot() {
    SDK::UWorld* world = ue::World();
    if (!world) return -1;
    SDK::UGameInstance* gi = world->OwningGameInstance;
    if (!gi || !ue::IsValid(gi)) return -1;

    // FS_GameInstance_C is a Blueprint class; resolve the function by name so we never need FS_GameInstance_functions.cpp.
    static SDK::UClass*    s_cachedClass = nullptr;
    static SDK::UFunction* s_fn = nullptr;
    if (gi->Class != s_cachedClass) {
        s_cachedClass = gi->Class;
        s_fn = ue::FindFunction(gi, "GetActiveSaveSlotIndex");
        if (!s_fn) QD_DEBUG("sync", "GetActiveSaveSlotIndex not found on {}", ue::ClassName(gi));
    }
    if (!s_fn) return -1;

    SDK::Params::FS_GameInstance_C_GetActiveSaveSlotIndex parms{};
    ue::CallFunction(gi, s_fn, &parms);
    return parms.ReturnValue;
}

}  // namespace qd::sync
