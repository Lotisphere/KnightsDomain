// Knights' Domain: enemy projectiles and area attacks (arrows, fireballs, flame breath, puddles ...). The machine
// that runs an enemy tells the others about every actor that enemy's Blueprint spawns; they spawn the same class at
// the same place, so it is visible and can hit the players on every machine. Game thread only.
#pragma once

namespace qd::sync {

void HazardSyncInit();       // after EnemySyncInit
void HazardSyncShutdown();

}  // namespace qd::sync
