// Knights' Domain: shared world state: saved world objects (chests, doors, levers, lifts, secret walls, barricades,
// pickups), story counters, visited save points, unlocked teleporters and the party checkpoint. Game thread only.
#pragma once

namespace qd::sync {

void WorldSyncInit();       // message handler + ticker; call after coop::Init
void WorldSyncShutdown();

}  // namespace qd::sync
