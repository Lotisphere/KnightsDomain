// QD Coop — player state sync: local sampling at 20 Hz, remote snapshot buffers + interpolation, puppets,
// overhead name labels. Held weapon (MsgEquip) + the queries enemy sync needs. Init/Shutdown/Tick are game-thread only;
// the Debug*/stats functions are thread-safe (menu).
#pragma once
#include "qdcoop/net/Protocol.h"

#include <cstdint>

namespace SDK { class AActor; }

namespace qd::sync {

void PlayerSyncInit();       // after coop::Init(): registers message handlers, member listener and the per-frame ticker
void PlayerSyncShutdown();   // destroys puppets

struct PlayerSyncStats {
    int puppets = 0;         // remote puppets currently spawned
    int remotes = 0;         // remote slots with fresh snapshots
    uint16_t localAreaId = 0;
    float capsuleHalfHeight = 0;
};
PlayerSyncStats GetPlayerSyncStats();   // thread-safe

// Debug: a puppet 3 m in front of the local player, driven by the menu (to tune mesh yaw / anims without a 2nd instance).
// it holds the local player's current weapon, so the weapon fit sliders can be tuned on it.
void DebugSetDummy(bool enabled);
void DebugSetDummyAction(uint16_t actionId, float speed2D);
bool DebugDummyEnabled();
uint16_t DebugDummyAction();
float DebugDummySpeed();

}  // namespace qd::sync

// queries for enemy sync (EnemySync / Rules / Summon). Game thread only.
namespace qd::playersync {

// The remote player's avatar actor in the current world; nullptr if not spawned (no fresh state, other area, local slot,
// title screen, spawn failed). Do not keep it across frames: puppets are destroyed/re-spawned freely.
SDK::AActor* PuppetActor(uint8_t slot);

// Latest interpolated pose of a remote player (the same one the puppet is drawn at this frame): `feet` = capsule bottom
// in world cm (actor location - capsule half height), `yaw` = control yaw in degrees (-180..180, without the mesh offset).
// false when there is no usable pose: local slot, slot not present, no snapshot for 2 s, other area (areaId differs),
// not in a session, or the local player is not in the game world. Valid even if the puppet itself failed to spawn.
bool RemotePose(uint8_t slot, net::Vec3f& feet, float& yaw);

}  // namespace qd::playersync
