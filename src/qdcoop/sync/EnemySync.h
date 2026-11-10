// QD Coop — enemy sync: enemy ids, per-enemy ownership (the host assigns every enemy
// to the player it is fighting; that machine runs the real AI and sends snapshots/events, every other machine — the host
// included — follows them with the AI off), damage routing to the owner, HP scaling, client spawner interception.
// Init/Shutdown/tick are game-thread only; GetEnemySyncStats() is thread-safe (menu / render thread).
#pragma once
#include "qdcoop/net/Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SDK { class AActor; class UClass; }

// ---- Interface enemy sync needs from PlayerSync. Implemented in PlayerSync.cpp; declared here so
// ---- EnemySync.cpp / Summon.cpp can call it. Game thread only.
namespace qd::playersync {
// Returns the puppet actor of a remote slot (nullptr if not spawned). (EnemySync no longer uses it.)
SDK::AActor* PuppetActor(uint8_t slot);
// Latest interpolated feet position/yaw of a remote slot; false if unknown.
bool RemotePose(uint8_t slot, net::Vec3f& feet, float& yaw);
}  // namespace qd::playersync

namespace qd::sync {

void EnemySyncInit();       // after coop::Init + ui::Init: message handlers, hooks, ticker, debug tab
void EnemySyncShutdown();   // releases controlled enemies, restores EnemyData HP, removes hooks

struct EnemySyncStats {
    bool    inSession = false;
    bool    host = true;
    uint8_t localSlot = 0;
    int     registered = 0;   // entries in the id map (spawners + enemies + remote-only ids)
    int     simulated = 0;    // live enemies this machine owns (in its last EnemyStates)
    int     followed = 0;     // enemies driven from another owner's snapshots this frame
    int     controlled = 0;   // enemies with AI disabled (followers)
    int     playerCount = 1;
    float   multNormal = 1, multElite = 1, multBoss = 1;
    std::vector<std::string> recentEvents;   // newest last, at most 5
    std::vector<std::string> recentOwners;   // owner changes, newest last, at most 5
};
EnemySyncStats GetEnemySyncStats();   // thread-safe

// ---- helpers shared with Rules.cpp / Summon.cpp (game thread) ----

// Remote player info tracked from PlayerState: false if this slot has sent nothing for 2 s.
bool RemotePlayerInfo(uint8_t slot, bool& dead, bool& sameArea);
// Fnv1a64(world name) & 0xFFFF — same formula as PlayerSync's areaId.
uint16_t LocalAreaId();
// All actors of `cls` (and subclasses) in the gameplay world. Uses UGameplayStatics::GetAllActorsOfClass with a
// preallocated buffer, so no engine memory is leaked per call.
void ActorsOfClass(SDK::UClass* cls, std::vector<SDK::AActor*>& out);

#if QD_DEV
// ---- test tooling (dev flavour only; debug/DevCommands.cpp). `filter` = case-insensitive part of the enemy's class or
// ---- actor name; the nearest match to the local player is used. Results go to the log with tag [edbg].
void DebugEnemyList(const std::string& filter);                 // one line per matching enemy
bool DebugEnemyGoto(const std::string& filter, float dist);     // teleport next to it, facing it
void DebugEnemyHp(const std::string& filter, float fraction);   // owner only: HP = fraction x max
void DebugEnemyHit(const std::string& filter, float damage);    // a hit by the local player (routed like a real one)
void DebugEnemyWake(const std::string& filter);                 // ActivateTrap on our copy (like walking up to it)
void DebugEnemyInteract(const std::string& filter);             // Interact(local player) on our copy
#endif

}  // namespace qd::sync
