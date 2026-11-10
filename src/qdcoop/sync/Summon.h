// QD Coop — "teleport to the host": F5 -> 传送 tab for clients, plus the "back at your save
// point" toast after a client dies and reloads. Init/Shutdown/tick on the game thread.
#pragma once

namespace qd::sync {

void SummonInit();       // menu tab + ticker; call after ui::Init
void SummonShutdown();

}  // namespace qd::sync
