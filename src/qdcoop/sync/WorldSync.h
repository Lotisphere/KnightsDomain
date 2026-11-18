// Knights' Domain: shared world state: saved world objects (chests, doors, levers, lifts, secret walls, barricades,
// pickups), story counters, visited save points, unlocked teleporters and the party checkpoint. Game thread only.
#pragma once

namespace qd::sync {

void WorldSyncInit();       // message handler + ticker; call after coop::Init
void WorldSyncShutdown();
// True while an event received from another machine runs here (its loot rolls are skipped: the drop comes from there).
bool WorldSyncReplaying();
// True when a received event ran on this object in the last `seconds` (a pot broken by another player rolls its loot
// at the end of its break animation, after the replay itself has returned).
bool WorldSyncReplayedRecently(const void* obj, double seconds);

}  // namespace qd::sync
