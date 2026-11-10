// QD Coop — small game-state queries that need SDK headers, kept out of SDK-free modules. Game thread only.
#pragma once

namespace qd::sync {

// UFS_GameInstance_C::GetActiveSaveSlotIndex() through reflection. -1 when there is no FS game instance / function.
int QueryActiveSaveSlot();

}  // namespace qd::sync
