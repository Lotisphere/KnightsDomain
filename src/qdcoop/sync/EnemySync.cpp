// QD Coop — enemy sync with per-enemy ownership. See EnemySync.h
// (construction records at the end of both).
//
// Ownership model in one paragraph: every registered enemy has an owner slot (default: the host, slot 0). The owner's machine
// runs the enemy's real AI — there "player 0" is the person it fights, so every BP / C++ path that reads
// GetPlayerPawn(0) is naturally right — and sends EnemyStates / EnemyEvent for it. Every other machine, the host
// included, disables the AI and follows those snapshots. Only the host assigns owners (EnemyOwner), every 0.5 s:
// last hitter within 6 s > keep while engaged > nearest player (3 s debounce). Damage always settles on the owner.
//
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/GameplayTags_structs.hpp"
#include "SDK/GameplayTags_classes.hpp"
#include "SDK/GameplayTags_parameters.hpp"
#include "SDK/FBAttributes_structs.hpp"
#include "SDK/FBAttributes_classes.hpp"
#include "SDK/FBAttributes_parameters.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/QueensDomain_parameters.hpp"
#include "SDK/EnemyData_classes.hpp"
#include "SDK/EnemyBase_classes.hpp"
#include "SDK/EnemyBase_parameters.hpp"
#include "SDK/EnemySpawner_classes.hpp"
#include "SDK/EnemySpawner_parameters.hpp"
#include "SDK/FS_PlayerChar_classes.hpp"
#include "SDK/FS_PlayerChar_parameters.hpp"

#include "qdcoop/sync/EnemySync.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qd::sync {
namespace {

constexpr const char* kTag = "enemysync";

// ---- classes (resolved lazily with ue::FindClass; BP classes may load late)
constexpr const char* kEnemyBaseClass = "/Game/Enemies/EnemyBase.EnemyBase_C";
constexpr const char* kSpawnerClass   = "/Game/Enemies/EnemySpawner.EnemySpawner_C";
constexpr const char* kPlayerClass    = "/Game/Player/FS_PlayerChar.FS_PlayerChar_C";

// ---- hooked functions (full UFunction paths)
// the GetTargetPlayer / GetDistanceToPlayerForAI rewrite hooks are gone — under ownership the simulating machine's
// player 0 IS the target, nothing needs rewriting (and nothing is logged from there either).
constexpr const char* kFnStartAttackState = "/Script/QueensDomain.FS_EnemyCharacterBase:BP_StartAttackState";
constexpr const char* kFnTryApplyDamage   = "/Game/Enemies/EnemyBase.EnemyBase_C:TryApplyDamage";
constexpr const char* kFnManagedDied      = "/Game/Enemies/EnemySpawner.EnemySpawner_C:ManagedEnemyDied";
constexpr const char* kFnBossBarOn        = "/Game/Player/FS_PlayerChar.FS_PlayerChar_C:ActivateBossHPBar";
constexpr const char* kFnBossBarOff       = "/Game/Player/FS_PlayerChar.FS_PlayerChar_C:DeactivateBossHPBar";
// Trap mode (EnemyBase.InTrapMode): skeletons lying in pieces, the Grave Guardian kneeling at its shrine. Only the
// machine whose copy rises calls ActivateTrap, so the rise is shared (EnemyEvent Woke + EnemyState flag bit4).
constexpr const char* kFnActivateTrap     = "/Game/Enemies/EnemyBase.EnemyBase_C:ActivateTrap";
// BP attack functions that play their own montage and drive their own hitboxes from montage callbacks. Replaying the
// generic attack montage (PlayAttack) on a follower shows the swing but deals no damage, so the owner sends
// CustomAttack(index) and followers run the same function on their copy. Index = position in this table (wire value).
struct CustomAttackFn { const char* path; const char* name; };
constexpr CustomAttackFn kCustomAttacks[] = {
    {"/Game/Enemies/Boner_00/Skeletons24/ShrineBoss/Boner_Shrineboss_BP.Boner_Shrineboss_BP_C:DoNormalAttack", "DoNormalAttack"},
    {"/Game/Enemies/Boner_00/Skeletons24/ShrineBoss/Boner_Shrineboss_BP.Boner_Shrineboss_BP_C:DoBigAttack", "DoBigAttack"},
};
constexpr int kCustomAttackCount = static_cast<int>(sizeof(kCustomAttacks) / sizeof(kCustomAttacks[0]));
// enemies the player talks to / interacts with to start the encounter (tools/enemy_scan.py, flag INTERACT).
constexpr const char* kInteractFns[] = {
    "/Game/Enemies/Boner_00/Skeletons24/Black/Mage/Boner_Black_Mage_BP.Boner_Black_Mage_BP_C:Interact",
    "/Game/Enemies/Boner_00/Skeletons24/ShrineBoss/Boner_Shrineboss_BP.Boner_Shrineboss_BP_C:Interact",
    "/Game/Enemies/Boner_00/Skeletons24/Standard/LostLibrarian_BP.LostLibrarian_BP_C:Interact",
    "/Game/Enemies/FishWifeEvil/FishWifeEvil_BP.FishWifeEvil_BP_C:Interact",
    "/Game/Enemies/Golem/Golem_WhiteTree_BP.Golem_WhiteTree_BP_C:Interact",
    "/Game/Enemies/JosamiKnight_00/JosamiKnight_Base.JosamiKnight_Base_C:Interact",
    "/Game/Enemies/JosamiLord/BloodSisters/JosamiBloodLord_Gun_BP.JosamiBloodLord_Gun_BP_C:Interact",
    "/Game/Enemies/JosamiLord/BloodSisters/JosamiBloodLord_Sword_BP.JosamiBloodLord_Sword_BP_C:Interact",
    "/Game/Enemies/JosamiLord/JosamiLord_BP.JosamiLord_BP_C:Interact",
    "/Game/Enemies/LargeRobot/LargeRobot_Rusty_BP.LargeRobot_Rusty_BP_C:Interact",
    "/Game/Enemies/MechaKnight/MechaKnight_DualAxe_Mossy_BP.MechaKnight_DualAxe_Mossy_BP_C:Interact",
    "/Game/Enemies/MechaKnight/MechaKnight_MausLance.MechaKnight_MausLance_C:Interact",
    "/Game/Enemies/Necronomicon/Necronomicon_BP.Necronomicon_BP_C:Interact",
    "/Game/Enemies/Orbiter_00/Orbiter_BP.Orbiter_BP_C:Interact",
    "/Game/Enemies/Queen/Queen_BP.Queen_BP_C:Interact",
    "/Game/Enemies/RoyalDuo/Fated_Wheeler_Miniboss_BP.Fated_Wheeler_Miniboss_BP_C:Interact",
    "/Game/Enemies/Sipeha/Sipeha_BP.Sipeha_BP_C:Interact",
    "/Game/Enemies/WhaleKing/WhaleKing_SG/Whaleking_SZ_BP.Whaleking_SZ_BP_C:Interact",
    "/Game/Enemies/WhaleKing/Whaleking_Boss_BP.Whaleking_Boss_BP_C:Interact",
};
constexpr const char* kSpawnerBlocked[] = {
    "/Game/Enemies/EnemySpawner.EnemySpawner_C:Respawn",
    "/Game/Enemies/EnemySpawner.EnemySpawner_C:RespawnEnemy",
    "/Game/Enemies/EnemySpawner.EnemySpawner_C:StartRespawnTimer",
    "/Game/Enemies/EnemySpawner.EnemySpawner_C:PlayerRested",
    "/Game/Enemies/EnemySpawner.EnemySpawner_C:Reset",
    "/Game/Enemies/EnemySpawner.EnemySpawner_C:Force Respawn",
};

// ---- tuning
// an enemy whose state changed since it was last sent goes out at 20 Hz; one that did not (standing, lying in
// trap mode, a dead spawner on the host) only every 0.5 s. Most enemies near a player are idle, so this cuts the
// enemy traffic by well over half while the ones that matter update twice as often as before (was a flat 10 Hz).
constexpr float  kStateInterval    = 0.05f;     // simulator snapshot tick, 20 Hz
// Measured in the starting area: ~126 entries within 60 m of the players, almost all idle. A flat 10 Hz would
// be ~45 KB/s; idle at 2 Hz still was 9.1 KB/s. Idle live enemies now go once a second, dead / unspawned spawner
// entries (host only) every 4 s; anything that changes is still sent on the next 20 Hz tick.
constexpr double kIdleStateInterval = 1.0;      // unchanged live enemies: 1 Hz (the "owner silent" limits are 3 s)
constexpr double kDeadStateInterval = 4.0;      // unchanged dead / unspawned entries
// Second measurement: with idle enemies throttled the host still sent 8.5 KB/s — ~9 enemies that wander or turn all
// the time (not fighting anyone) were "changed" on every tick. An enemy that only moved / turned and is not engaged
// goes at 5 Hz; followers stretch their interpolation delay for it (FollowDelay). Engaged enemies and any change of
// HP / flags / state / target still go on the next 20 Hz tick.
constexpr double kWanderStateInterval = 0.2;
constexpr double kGapAnchorSec     = 0.45;      // follower: a snapshot after a longer gap gets an anchor (see AcceptStates)
constexpr float  kRebuildInterval  = 0.5f;      // id map rebuild
constexpr float  kIdleRebuildInterval = 3.0f;   // ... while not in a session (solo play pays almost nothing)
constexpr float  kOwnerEvalInterval = 0.5f;     // host: ownership evaluation
constexpr float  kStatsInterval    = 0.25f;
constexpr float  kRelevantRange    = 6000.0f;   // cm: enemies within 60 m of any player are synced / evaluated
constexpr double kHitAggroSec      = 6.0;       // rule 1: the last hitter within this window owns the enemy
constexpr double kOwnerSwitchSec   = 6.0;       // rule 4: min. time between two owner changes (not for hits)
// Measured in a play test (662 changes, up to 19 per enemy): proximity hand-over needs a clear winner.
constexpr float  kNearAssignRange  = 3000.0f;   // cm: "nearest" only hands an enemy to a player within 30 m of it
constexpr float  kNearMarginCm     = 800.0f;    // ... who is at least 8 m closer than the current owner
constexpr float  kNearMarginRatio  = 0.7f;      // ... and at most 70 % of the current owner's distance
constexpr double kHitSwitchMinSec  = 1.0;       // hits: floor between two changes
// bosses / elites keep private Blueprint state per copy (phase counters, intro flags). Every hand-over risks a
// repeated phase change, so they change owner rarely: a hit moves them at most every 12 s, proximity never while the
// owner is alive and near.
constexpr double kBossHitSwitchSec = 12.0;
constexpr double kBlobInterval     = 1.0;       // owner: boss state blob cadence (only between attacks)
constexpr double kBlobFreshSec     = 60.0;      // takeover: a stored blob older than this is not applied
constexpr double kInteractNoticeSec = 8.0;      // at most one Interacted notice per enemy per 8 s
constexpr double kOwnerStaleSec    = 3.0;       // host: an owner silent this long loses the enemy ("失联")
constexpr double kStaleExcludeSec  = 10.0;      // ... and is not picked as "nearest" for it again for this long
constexpr double kOwnerGraceSec    = 1.0;       // a Died from the previous owner is still accepted this long
constexpr double kAlignFreshSec    = 1.3;       // takeover: align to the last snapshot only if it is this fresh (idle = 2 Hz)
constexpr double kHitNotifySec     = 0.5;       // client simulator: at most one HitNotifyOnly per enemy per 0.5 s
constexpr double kInterpDelay      = 0.075;     // render enemies 75 ms in the past (1.5 snapshot intervals at 20 Hz)
constexpr float  kSnapDistance     = 1000.0f;   // cm: larger jumps are not interpolated
constexpr size_t kSnapMax          = 16;
constexpr double kOwnerSilentSec   = 3.0;       // follower: no snapshot for this long -> stop driving the enemy
constexpr double kRemoteStaleSec   = 3.0;
constexpr float  kFeetToCenter     = 90.0f;     // remote poses are feet; compare against capsule centres
constexpr float  kMaxDamage        = 5000.0f;
constexpr size_t kMaxTags          = 32;
constexpr float  kShieldHp         = 1.0e9f;
constexpr double kDeadGraceSec     = 1.0;       // owner says dead without a Died event yet -> wait, then Kill(NoRewards)
constexpr double kAliveConfirmSec  = 0.5;       // owner must report "alive" this long before a client Force Respawns
constexpr double kRespawnRetrySec  = 5.0;
constexpr double kKillRetrySec     = 2.0;
constexpr double kSpawnerAllowSec  = 6.0;       // our own Force Respawn may chain into Respawn/RespawnEnemy/timers
constexpr double kEntryStaleSec    = 5.0;
constexpr double kMaxHpRefreshSec  = 1.0;
constexpr double kAttackDedupSec   = 0.25;
constexpr int    kActorBufferCap   = 16384;     // GetAllActorsOfClass output buffer (never reallocated by the engine)
constexpr size_t kInboxMax         = 4096;

enum Tier : uint8_t { kNormal = 0, kElite = 1, kBoss = 2 };

using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

bool Valid(const SDK::UObject* o) { return o && ue::IsValid(o); }

float Dist(const SDK::FVector& a, const SDK::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
float NormalizeAxis(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0) a += 360.0f;
    return a - 180.0f;
}
float LerpAngle(float a, float b, float t) { return NormalizeAxis(a + NormalizeAxis(b - a) * t); }
SDK::FVector ToF(const net::Vec3f& v) { return SDK::FVector(v.x, v.y, v.z); }
net::Vec3f ToN(const SDK::FVector& v) { return {v.X, v.Y, v.Z}; }
float NDist(const net::Vec3f& a, const net::Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
unsigned ShortId(uint64_t id) { return static_cast<unsigned>(id & 0xFFFF); }

// ------------------------------------------------------------------ state

struct Snap {
    double t = 0;   // local receive time
    net::EnemyState s;
};

struct Entry {
    uint64_t id = 0;
    SDK::AEnemySpawner_C* spawner = nullptr;
    SDK::AEnemyBase_C*    enemy = nullptr;
    bool seenSpawner = false, seenEnemy = false;   // rebuild bookkeeping
    // ---- simulator side (this machine owns the enemy)
    bool        relevant = false;                  // inside 60 m of some player at the last state tick (owned only)
    const void* diedSentFor = nullptr;             // Died already sent (or never to be sent) for this enemy instance
    bool        prevStaggered = false;
    bool        bossBar = false;                   // ActivateBossHPBar seen for this enemy
    float       maxHp = 0;                         // local GetMaxHP cache
    double      maxHpAt = -100;
    double      lastAttackAt = -100;
    int32_t     lastAttackIdx = -1;
    double      lastNotifyAt = -100;               // client simulator: last HitNotifyOnly sent
    // ---- host: ownership inputs
    uint8_t lastHitSlot = 0xFF;
    double  lastHitAt = -100;
    uint8_t excludeSlot = 0xFF;                    // owner that went silent: not "nearest" again until excludeUntil
    double  excludeUntil = 0;
    // ---- follower side (another machine owns the enemy)
    std::deque<Snap> snaps;                        // from the current owner only
    double seenAt = -100;                          // last accepted snapshot
    double ownerAliveSince = -1;
    double ownerDeadSince = -1;
    bool   ownerDiedEvent = false;
    bool   controlled = false;
    SDK::AEnemyBase_C* controlledEnemy = nullptr;
    SDK::EMovementMode savedMoveMode = SDK::EMovementMode::MOVE_Walking;
    double nextReconcileAt = 0;
    double clientMaxAt = -100;
    // ---- trap mode / custom attacks (every machine)
    const void* wokenFor = nullptr;                // this enemy instance has left trap mode here (ActivateTrap ran)
    bool        customAttacks = false;             // uses kCustomAttacks: the generic AttackStart is not sent for it
    const void* customCheckedFor = nullptr;        // instance customAttacks was computed for
    // ---- boss state blob
    std::vector<uint8_t> blob;                     // latest blob from the owner (or our own last one)
    uint32_t    blobLayout = 0;
    double      blobAt = -100;
    double      blobSentAt = -100;
    double      interactSentAt = -100;
    int16_t     stateSet = -1, subSet = -1;        // last state index / sub-state (phase) we pushed onto our copy
    // ---- montage mirror
    net::EnemyState sentState;                     // owner: the state we sent last (idle throttling)
    bool        sentHave = false;
    double      sentAt = -100;
    double      followDelay = kInterpDelay;        // follower: current interpolation delay for this enemy
    const void* lastMontage = nullptr;             // owner: montage reported last (UAnimMontage*, identity only)
    std::string mirrorPath;                        // follower: owner's current montage ("" = none)
    double      mirrorAt = -100;
};

// Ownership of one enemy id. Kept outside Entry so it survives world changes and entry pruning (a snapshot from the
// owner must never be rejected just because the local entry was rebuilt). Missing = owned by the host.
struct OwnerRec {
    uint8_t slot = 0;
    uint8_t prev = 0;
    double  changedAt = -100;
};

struct Cand {
    uint8_t      slot = 0;
    SDK::FVector pos;
    bool         alive = true;
};

struct RemoteTrack {
    bool     has = false;
    bool     dead = false;
    uint16_t area = 0;
    double   at = -100;
};

struct SeqTrack {
    bool     has = false;
    uint16_t last = 0;
};

struct DataInfo {
    float   orig = 0;       // EnemyData.HP before we touched it
    uint8_t tier = kNormal;
};

struct DamageTemplate {
    SDK::FFB_DamageAttributeSet set;      // engine-owned containers, kept for the whole process (bounded cache)
    std::vector<SDK::FName>     dataNames;  // same order as the request's dataTags
};

struct ShieldFrame {
    const SDK::UObject* enemy = nullptr;
    bool  active = false;
    float savedHp = 0;
};

struct Classes {
    SDK::UClass* enemyBase = nullptr;
    SDK::UClass* spawner = nullptr;
    SDK::UClass* player = nullptr;
};

using RawHandler = void (*)(uint8_t fromSlot, const uint8_t* data, size_t size);
struct Deferred {
    RawHandler           fn = nullptr;
    uint8_t              from = 0;
    std::vector<uint8_t> bytes;
};

struct OwnerChange {
    uint64_t    id;
    uint8_t     slot;
    const char* reason;
    bool        align;
};

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    std::vector<hooks::HookId> hookIds;

    SDK::UWorld* world = nullptr;
    bool    active = false;   // coop::InSession()
    bool    wakingLocal = false;       // inside our own ActivateTrap call (don't echo a Woke)
    int     customDepth = 0;           // nesting of kCustomAttacks calls (only the outermost is sent)
    bool    replayingCustom = false;   // inside a follower replay of a custom attack
    bool    isHost = true;
    uint8_t localSlot = 0;
    Classes cls;

    std::unordered_map<uint64_t, Entry>          entries;
    std::unordered_map<uint64_t, OwnerRec>       owners;   // session-scoped; survives world changes
    std::unordered_map<const void*, uint64_t>    byEnemy;
    std::unordered_map<const void*, uint64_t>    spawnerIds;
    std::unordered_map<uint64_t, const void*>    idOwner;   // id -> spawner (duplicate GUID detection)
    std::unordered_map<const void*, double>      spawnerAllowUntil;
    std::unordered_map<SDK::UEnemyData_C*, DataInfo> datas;   // NOT cleared on world change (assets outlive worlds)
    std::unordered_map<const void*, uint8_t>     classTier;
    std::map<std::pair<const void*, std::string>, SDK::UFunction*> fnCache;
    std::unordered_map<std::string, std::unique_ptr<DamageTemplate>> damageCache;
    std::vector<ShieldFrame> shield;
    std::vector<Cand>        cands;
    std::array<RemoteTrack, net::kMaxPlayers> remotes{};
    std::array<SeqTrack, net::kMaxPlayers>    seqs{};      // EnemyStates seq per sending owner
    std::vector<Deferred>    inbox;                        // messages that arrived while a hook callback was running
    std::vector<uint8_t>     pendingOwnerSync;             // host: newcomers that still need the owner table
    std::vector<const void*> hostSpawnerResets;            // host: spawners that ran PlayerRested / Reset

    SDK::UWorld* areaWorld = nullptr;
    uint16_t     areaId = 0;

    float    rebuildAccum = kRebuildInterval, stateAccum = 0, ownerAccum = 0, statsAccum = 0;
    uint16_t seq = 0;
    int      lastPlayerCount = 0;
    bool     resendHpScale = false;
    bool     rescalePending = false;   // client: a new HpScale arrived
    net::MsgHpScale hpScale;
    int      simulated = 0;
    int      followed = 0;
    int      controlledCount = 0;
    bool     scaleVerified = false;
    int      blockedLogs = 0;
    bool     applyingRemote = false;
    std::unique_ptr<SDK::Params::EnemyBase_C_TryApplyDamage> tadParams;   // reused: AppliedDamage keeps its engine allocation
    double   startTime = 0;
    int      hookDepth = 0;   // >0 while one of our hook callbacks / message handlers runs (see HookScope)
};
State& S() {
    static State s;
    return s;
}

// Any ProcessEvent made inside a hook callback can run gamethread::Pump() and with it our ticker (and coop's, which
// dispatches network messages), which rebuild the maps the callback may be holding iterators into. Hook callbacks and
// message handlers open a HookScope; Tick() skips a frame while one is open (the next frame catches up) and handlers
// that arrive inside one are queued in S().inbox.
struct HookScope {
    HookScope() { ++S().hookDepth; }
    ~HookScope() { --S().hookDepth; }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;
};

const OwnerRec& OwnerOf(uint64_t id) {
    static const OwnerRec kHostOwned{};
    const auto& o = S().owners;
    auto it = o.find(id);
    return it == o.end() ? kHostOwned : it->second;
}
bool IsMine(uint64_t id) { return OwnerOf(id).slot == S().localSlot; }

bool MemberPresent(uint8_t slot) {
    if (slot >= net::kMaxPlayers) return false;
    const coop::Member& m = coop::Members()[slot];
    return m.present;
}

// ------------------------------------------------------------------ stats / event log (thread-safe)

std::mutex              g_statMutex;
EnemySyncStats          g_stats;
std::deque<std::string> g_events;
std::deque<std::string> g_owners;

const char* EventName(net::EnemyEventType t) {
    switch (t) {
        case net::EnemyEventType::AttackStart: return "攻击";
        case net::EnemyEventType::Staggered:   return "硬直";
        case net::EnemyEventType::Died:        return "死亡";
        case net::EnemyEventType::BossBarOn:   return "Boss血条开";
        case net::EnemyEventType::BossBarOff:  return "Boss血条关";
        case net::EnemyEventType::Woke:        return "苏醒";
        case net::EnemyEventType::CustomAttack: return "专属攻击";
        case net::EnemyEventType::Interacted:  return "互动";
    }
    return "?";
}

void PushLine(std::deque<std::string>& q, const std::string& line) {
    const std::string stamped = std::format("{:7.1f}s {}", Now() - S().startTime, line);
    std::lock_guard lk(g_statMutex);
    q.push_back(stamped);
    while (q.size() > 5) q.pop_front();
}
void LogEvent(const std::string& line) { PushLine(g_events, line); }

void LogOwner(uint64_t id, uint8_t from, uint8_t to, const char* reason) {
    QD_INFO(kTag, "owner {:016X}: P{} -> P{} ({})", id, from + 1, to + 1, reason);
    PushLine(g_owners, std::format("{:04X} P{} → P{} {}", ShortId(id), from + 1, to + 1, reason));
}

// ------------------------------------------------------------------ reflection helpers

SDK::UFunction* Fn(SDK::UObject* obj, const char* name) {
    if (!Valid(obj) || !obj->Class) return nullptr;
    auto& cache = S().fnCache;
    const auto key = std::make_pair(static_cast<const void*>(obj->Class), std::string(name));
    if (auto it = cache.find(key); it != cache.end()) {
        if (!it->second || Valid(it->second)) return it->second;
    }
    SDK::UFunction* f = ue::FindFunction(obj, name);
    if (!f) QD_WARN(kTag, "function '{}' not found on {}", name, ue::ClassName(obj));
    cache[key] = f;
    return f;
}

template <class P>
bool Call(SDK::UObject* obj, const char* name, P& parms) {
    SDK::UFunction* f = Fn(obj, name);
    if (!f) return false;
    ue::CallFunction(obj, f, &parms);
    return true;
}

void RefreshClasses() {
    auto& c = S().cls;
    if (!Valid(c.enemyBase)) c.enemyBase = ue::FindClass(kEnemyBaseClass);
    if (!Valid(c.spawner)) c.spawner = ue::FindClass(kSpawnerClass);
    if (!Valid(c.player)) c.player = ue::FindClass(kPlayerClass);
}

bool IsFsPlayer(const SDK::UObject* a) {
    SDK::UClass* pc = S().cls.player;
    return a && Valid(pc) && a->IsA(pc);
}
bool LocalDead(SDK::ACharacter* ch) {
    return IsFsPlayer(ch) && static_cast<SDK::AFS_PlayerChar_C*>(ch)->IsDead;
}

float QueryMaxHp(SDK::AEnemyBase_C* en) {
    SDK::Params::EnemyBase_C_GetMaxHP p{};
    if (!Call(en, "GetMaxHP", p)) return 0;
    return std::isfinite(p.HP_0) ? p.HP_0 : 0;
}

// Local GetMaxHP, refreshed at most once a second.
float CachedMaxHp(Entry& e, SDK::AEnemyBase_C* en, double now) {
    if (now - e.maxHpAt > kMaxHpRefreshSec) {
        e.maxHpAt = now;
        e.maxHp = QueryMaxHp(en);
    }
    return e.maxHp;
}

void KillEnemy(SDK::AEnemyBase_C* en, bool noRewards, bool suppressSound) {
    if (!Valid(en) || en->bDead) return;
    SDK::Params::EnemyBase_C_Kill p{};
    p.NoRewards = noRewards;
    p.SupressDeathSound = suppressSound;
    Call(en, "Kill", p);
}

bool IsPreview(const SDK::AEnemyBase_C* en) {
    return en->bSpawnerPreviewInstance || en->BestiaryInstance || en->TempEnemyInstance;
}

// Ownership rule 2 on a live, locally simulated enemy (SDK: AFS_EnemyCharacterBase).
bool EngagedLive(const SDK::AEnemyBase_C* en) { return en->bAggrod || en->bIsChasing || en->bIsAttacking; }

// ------------------------------------------------------------------ HP scaling
// GetMaxHP/Init_HP report through an OUT parameter; a post hook only sees the frame's Locals copy (the real write
// goes to the caller through FFrame::OutParms), so the value cannot be changed from a hook.
// GetMaxHP's body reads EnemyData.HP (it only multiplies by the crown factor), so we scale the EnemyData asset
// instead: Init_HP, GetMaxHP, HP bars and the boss bar all follow. Live enemies keep their HP percentage.
// clients can simulate enemies too, so they scale their own EnemyData with the host's HpScale multipliers.

float PerPlayer(uint8_t tier) {
    const auto& h = config::Get().hpScale;
    return tier == kBoss ? h.boss : tier == kElite ? h.elite : h.normal;
}

float MultFor(uint8_t tier) {
    const auto& s = S();
    if (!s.active) return 1.0f;
    if (s.isHost) {
        const int n = std::clamp(coop::PlayerCount(), 1, net::kMaxPlayers);
        return std::max(0.1f, 1.0f + PerPlayer(tier) * static_cast<float>(n - 1));
    }
    const auto& h = s.hpScale;   // last HpScale from the host (1/1/1 until the first one arrives)
    const float m = tier == kBoss ? h.boss : tier == kElite ? h.elite : h.normal;
    return std::isfinite(m) ? std::max(0.1f, m) : 1.0f;
}

uint8_t ClassTier(const SDK::UObject* en) {
    auto& s = S();
    const void* cls = en->Class;
    if (auto it = s.classTier.find(cls); it != s.classTier.end()) return it->second;
    const std::string path = ue::PathName(en->Class);
    // Elite = minibosses and boss-class enemies without the EnemyData.Boss flag (e.g. Cocker_Boss_00, King_Fishman).
    // Boss adds (NecroZombie_BossAdd, Fisher_KingAdd) and bestiary dummies stay normal.
    const bool add = path.find("Add_") != std::string::npos || path.find("Bestiary") != std::string::npos;
    const bool elite = !add && (path.find("Miniboss") != std::string::npos || path.find("Duo") != std::string::npos ||
                                path.find("Boss") != std::string::npos || path.find("King_") != std::string::npos ||
                                path.find("Lord_") != std::string::npos);
    const uint8_t t = elite ? kElite : kNormal;
    s.classTier[cls] = t;
    return t;
}

DataInfo* RegisterData(SDK::UEnemyData_C* d) {
    if (!Valid(d)) return nullptr;
    auto [it, inserted] = S().datas.try_emplace(d);
    if (inserted) it->second.orig = d->HP;   // first sight: the asset is still untouched
    return &it->second;
}

bool IsBoss(const Entry& e, SDK::AEnemyBase_C* en) {
    if (e.bossBar) return true;
    SDK::UEnemyData_C* d = en->Data;
    if (!Valid(d)) return false;
    if (d->Boss) return true;
    auto it = S().datas.find(d);
    return it != S().datas.end() && it->second.tier == kBoss;
}

// Registers the enemy's data asset and raises its tier. True if something changed.
bool NoteEnemyTier(SDK::AEnemyBase_C* en, bool bossBar) {
    SDK::UEnemyData_C* d = en->Data;
    if (!Valid(d)) return false;
    const bool isNew = !S().datas.contains(d);
    DataInfo* info = RegisterData(d);
    if (!info) return false;
    uint8_t t = ClassTier(en);
    if (d->Boss || bossBar) t = kBoss;
    bool changed = isNew;
    if (t > info->tier) {
        info->tier = t;
        changed = true;
    }
    return changed;
}

// Both ends (was ApplyHostScaling): EnemyData.HP = original x MultFor(tier), live enemies keep HP %.
void ApplyScaling(const char* reason) {
    auto& s = S();
    struct Change { SDK::UEnemyData_C* d; float want; };
    std::vector<Change> changes;
    for (auto it = s.datas.begin(); it != s.datas.end();) {
        if (!Valid(it->first)) {
            it = s.datas.erase(it);
            continue;
        }
        const float want = it->second.orig * MultFor(it->second.tier);
        if (std::fabs(it->first->HP - want) > 0.001f * std::max(1.0f, want)) changes.push_back({it->first, want});
        ++it;
    }
    if (changes.empty()) return;

    struct Fix { Entry* e; SDK::AEnemyBase_C* en; float frac; float oldHp; float oldMax; };
    std::vector<Fix> fixes;
    for (auto& [id, e] : s.entries) {
        SDK::AEnemyBase_C* en = e.enemy;
        if (!Valid(en) || en->bDead) continue;
        const bool affected = std::any_of(changes.begin(), changes.end(), [&](const Change& c) { return c.d == en->Data; });
        if (!affected) continue;
        const float mx = QueryMaxHp(en);
        if (mx <= 0) continue;
        fixes.push_back({&e, en, std::clamp(en->HP / mx, 0.0f, 1.0f), en->HP, mx});
    }
    for (const Change& c : changes) c.d->HP = c.want;
    for (const Fix& f : fixes) {
        const float mx = QueryMaxHp(f.en);
        if (mx <= 0) continue;
        float hp = f.frac * mx;
        if (f.oldHp > 0 && hp < 1.0f) hp = 1.0f;
        f.en->HP = hp;
        f.e->maxHp = mx;
        f.e->maxHpAt = Now();
        if (!s.scaleVerified) {
            s.scaleVerified = true;
            if (std::fabs(mx - f.oldMax) < 0.01f)
                QD_WARN(kTag, "HP scaling: GetMaxHP did not change after scaling EnemyData.HP ({} -> {}); scaling has no effect "
                              "on this build", f.oldMax, mx);
            else
                QD_INFO(kTag, "HP scaling verified on {}: max {:.1f} -> {:.1f}", ue::Name(f.en), f.oldMax, mx);
        }
    }
    QD_INFO(kTag, "HP scaling ({}): {} EnemyData asset(s) set, {} live enemies rescaled (players {})", reason,
            changes.size(), fixes.size(), coop::PlayerCount());
}

// Shutdown / session end: put every EnemyData.HP back, clamp live HP to the restored maximum.
void RestoreAllData() {
    auto& s = S();
    int restored = 0;
    for (auto it = s.datas.begin(); it != s.datas.end();) {
        if (!Valid(it->first)) {
            it = s.datas.erase(it);
            continue;
        }
        if (it->first->HP != it->second.orig) {
            it->first->HP = it->second.orig;
            ++restored;
        }
        ++it;
    }
    if (!restored) return;
    for (auto& [id, e] : s.entries) {
        SDK::AEnemyBase_C* en = e.enemy;
        if (!Valid(en) || en->bDead) continue;
        const float mx = QueryMaxHp(en);
        if (mx > 0 && en->HP > mx) en->HP = mx;
    }
    QD_INFO(kTag, "restored {} EnemyData HP value(s)", restored);
}

// Client follower of a HOST-owned enemy: make the local GetMaxHP match the host's (authoritative) maximum, for the
// case where the two ends disagree on an enemy's tier. (Followers of client-owned enemies don't chase each other.)
void ClientMatchMaxHp(Entry& e, SDK::AEnemyBase_C* en, float hostMax, double now) {
    if (hostMax <= 0 || now - e.clientMaxAt < kMaxHpRefreshSec) return;
    e.clientMaxAt = now;
    SDK::UEnemyData_C* d = en->Data;
    if (!Valid(d)) return;
    const float cur = QueryMaxHp(en);
    if (cur <= 0 || std::fabs(cur - hostMax) <= 0.005f * hostMax) return;
    RegisterData(d);
    if (d->HP <= 0) return;
    d->HP = d->HP * hostMax / cur;
    e.maxHpAt = -100;   // refresh the cache
}

uint8_t TierOf(SDK::AEnemyBase_C* en);
bool    IsBossLike(const Entry& e, SDK::AEnemyBase_C* en) { return e.bossBar || TierOf(en) >= kElite; }

// ------------------------------------------------------------------ boss state blob
// The plain variables a boss Blueprint declares below EnemyBase_C: bool / int / float / byte / 1-byte enum. Timeline
// outputs (generated names ending in a GUID, "__Direction_") are skipped: they belong to a running timeline.

struct BlobField {
    int32_t offset = 0;
    uint8_t size = 0;      // bytes on the wire and in the object (bool: 1)
    uint8_t mask = 0;      // != 0: bool bit mask
    bool    isFloat = false;
};
struct BlobLayout {
    uint32_t hash = 0;
    size_t   bytes = 0;
    std::vector<BlobField> fields;
};

bool TimelineGenerated(const std::string& n) {
    if (n.find("__Direction_") != std::string::npos) return true;
    if (n.size() > 33 && n[n.size() - 33] == '_') {
        bool hex = true;
        for (size_t i = n.size() - 32; i < n.size() && hex; ++i) hex = std::isxdigit(static_cast<unsigned char>(n[i])) != 0;
        if (hex) return true;
    }
    return false;
}

std::unordered_map<const void*, BlobLayout>& BlobLayouts() {
    static std::unordered_map<const void*, BlobLayout> m;
    return m;
}

// nullptr when the class has nothing to sync (or EnemyBase_C is not resolved yet).
const BlobLayout* BlobLayoutFor(SDK::AEnemyBase_C* en) {
    SDK::UClass* base = S().cls.enemyBase;
    if (!Valid(base) || !en->Class) return nullptr;
    auto& cache = BlobLayouts();
    if (auto it = cache.find(en->Class); it != cache.end()) return it->second.fields.empty() ? nullptr : &it->second;
    BlobLayout l;
    uint64_t h = 1469598103934665603ull;
    int depth = 0;
    for (SDK::UStruct* st = en->Class; st && st != base && depth < 16; st = st->SuperStruct, ++depth) {
        for (SDK::FField* f = st->ChildProperties; f; f = f->Next) {
            if (!f->ClassPrivate) continue;
            const std::string type = ue::ToString(f->ClassPrivate->Name);
            auto* p = static_cast<SDK::FProperty*>(f);
            if (p->ArrayDim != 1 || p->Offset < 0) continue;
            BlobField bf;
            if (type == "BoolProperty") {
                auto* b = static_cast<SDK::FBoolProperty*>(f);
                if (b->ByteMask == 0) continue;
                bf.offset = p->Offset + b->ByteOffset;
                bf.size = 1;
                bf.mask = b->ByteMask;
            } else if (type == "IntProperty" || type == "FloatProperty") {
                bf.offset = p->Offset;
                bf.size = 4;
                bf.isFloat = type == "FloatProperty";
            } else if (type == "ByteProperty" || (type == "EnumProperty" && p->ElementSize == 1)) {
                bf.offset = p->Offset;
                bf.size = 1;
            } else {
                continue;
            }
            const std::string name = ue::ToString(f->Name);
            if (TimelineGenerated(name)) continue;
            l.fields.push_back(bf);
            l.bytes += bf.size;
            for (const char c : name) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
            h = (h ^ (static_cast<uint64_t>(bf.size) | (bf.mask ? 0x100u : 0u) | (bf.isFloat ? 0x200u : 0u))) * 1099511628211ull;
        }
    }
    if (l.bytes > net::kMaxEnemyBlobBytes) {
        QD_WARN(kTag, "{}: {} state bytes do not fit a blob, not syncing its Blueprint state", ue::ClassName(en), l.bytes);
        l.fields.clear();
        l.bytes = 0;
    }
    l.hash = static_cast<uint32_t>(h ^ (h >> 32));
    if (!l.fields.empty())
        QD_INFO(kTag, "{}: Blueprint state layout {:08X}: {} variables, {} bytes", ue::ClassName(en), l.hash, l.fields.size(), l.bytes);
    auto [it, _] = cache.emplace(en->Class, std::move(l));
    return it->second.fields.empty() ? nullptr : &it->second;
}

void CaptureBlob(SDK::AEnemyBase_C* en, const BlobLayout& l, std::vector<uint8_t>& out) {
    out.resize(l.bytes);
    const uint8_t* obj = reinterpret_cast<const uint8_t*>(en);
    size_t o = 0;
    for (const BlobField& f : l.fields) {
        if (f.mask) out[o] = (obj[f.offset] & f.mask) ? 1 : 0;
        else std::memcpy(&out[o], obj + f.offset, f.size);
        o += f.size;
    }
}

bool ApplyBlob(SDK::AEnemyBase_C* en, const BlobLayout& l, const std::vector<uint8_t>& data) {
    if (data.size() != l.bytes) return false;
    uint8_t* obj = reinterpret_cast<uint8_t*>(en);
    size_t o = 0;
    for (const BlobField& f : l.fields) {
        if (f.mask) {
            if (data[o]) obj[f.offset] |= f.mask;
            else obj[f.offset] &= static_cast<uint8_t>(~f.mask);
        } else if (f.isFloat) {
            float v = 0;
            std::memcpy(&v, &data[o], 4);
            if (std::isfinite(v)) std::memcpy(obj + f.offset, &v, 4);
        } else {
            std::memcpy(obj + f.offset, &data[o], f.size);
        }
        o += f.size;
    }
    return true;
}

// The stored blob onto our copy; `why` goes to the log (takeover only — the periodic apply is silent).
bool ApplyStoredBlob(Entry& e, SDK::AEnemyBase_C* en, const char* why) {
    if (e.blob.empty()) return false;
    const BlobLayout* l = BlobLayoutFor(en);
    if (!l || l->hash != e.blobLayout) return false;
    if (!ApplyBlob(en, *l, e.blob)) return false;
    if (why) QD_INFO(kTag, "{:016X} ({}): Blueprint state applied ({})", e.id, ue::Name(en), why);
    return true;
}

// ------------------------------------------------------------------ montage mirror
// Owner side, at the snapshot rate: report the montage our copy plays whenever it changes ("" when it stops).
void ReportMontage(Entry& e, SDK::AEnemyBase_C* en) {
    SDK::UAnimMontage* m = en->GetCurrentMontage();
    if (!Valid(m)) m = nullptr;
    if (m == e.lastMontage) return;
    e.lastMontage = m;
    net::MsgEnemyMontage msg;
    msg.enemyId = e.id;
    msg.ownerSlot = S().localSlot;
    if (m) msg.path = ue::PathName(m);
    if (msg.path.size() > net::kMaxMontagePathBytes) return;
    coop::Broadcast(net::Channel::Reliable, net::Encode(msg));
}

// Follower side: play the owner's montage on our copy unless it is already the one playing (the attack replay may
// have started the same asset a moment earlier).
void MirrorMontage(Entry& e, SDK::AEnemyBase_C* en, const std::string& path) {
    SDK::UAnimMontage* cur = en->GetCurrentMontage();
    if (!Valid(cur)) cur = nullptr;
    if (path.empty()) {
        // The owner's montage ended or was interrupted. Ours ends by itself; only cut one that is clearly a different
        // (longer / looping) animation the owner already left.
        return;
    }
    SDK::UObject* o = ue::FindObject(path);
    if (!o) o = ue::LoadObject(path);
    if (!o || !o->IsA(SDK::UAnimMontage::StaticClass())) {
        QD_DEBUG(kTag, "{:016X}: montage '{}' not found", e.id, path);
        return;
    }
    auto* montage = static_cast<SDK::UAnimMontage*>(o);
    if (cur == montage) return;
    en->PlayAnimMontage(montage, 1.0f, SDK::FName());
}

// Owner, about once a second per boss / elite, only between attacks (no montage playing): the variables are then in
// their resting state, so a copy that receives them is never left with "mid-attack" flags set.
void SendBlob(Entry& e, SDK::AEnemyBase_C* en, double now) {
    e.blobSentAt = now;
    const BlobLayout* l = BlobLayoutFor(en);
    if (!l) return;
    if (Valid(en->GetCurrentMontage())) return;
    net::MsgEnemyBlob m;
    m.enemyId = e.id;
    m.ownerSlot = S().localSlot;
    m.layout = l->hash;
    CaptureBlob(en, *l, m.data);
    e.blob = m.data;
    e.blobLayout = m.layout;
    e.blobAt = now;
    coop::Broadcast(net::Channel::Unreliable, net::Encode(m));
}

// Followers' HP is written straight into the enemy, so nothing tells the health-bar widgets (they listen to the
// enemy's TookDamage delegate): the boss bar stayed full until the local player landed a hit. Calls the widget
// listeners of that delegate only — gameplay listeners (boss scripts) must not see a hit that the owner already
// processed.
void NotifyHpWidgets(SDK::AEnemyBase_C* en, float damage) {
    static SDK::UClass* widgetCls = nullptr;
    if (!Valid(widgetCls)) widgetCls = ue::FindClass("/Script/UMG.UserWidget");
    if (!widgetCls) return;
    struct Listener { SDK::UObject* obj; SDK::FName fn; };
    Listener listeners[16];
    int n = 0;
    auto& list = en->TookDamage.InvocationList;
    for (int i = 0; i < list.Num() && n < 16; ++i) {
        SDK::UObject* o = list[i].Object.Get();
        if (!o || !Valid(o) || !o->IsA(widgetCls)) continue;
        listeners[n++] = {o, list[i].FunctionName};
    }
    for (int i = 0; i < n; ++i) {
        SDK::UFunction* fn = ue::FindFunction(listeners[i].obj, ue::ToString(listeners[i].fn));
        if (!fn) continue;
        // (AEnemyBase_C* Enemy, float Damage, FGameplayTagContainer Tags, TMap<FGameplayTag, float> DataTags):
        // 8 + 4 (+4) + 0x20 + 0x50 bytes; zeroed containers are valid empty ones.
        alignas(16) uint8_t parms[0x100] = {};
        std::memcpy(parms, &en, sizeof(en));
        std::memcpy(parms + 8, &damage, sizeof(damage));
        ue::CallFunction(listeners[i].obj, fn, parms);
    }
}

// ------------------------------------------------------------------ control (follower: AI off / simulator: AI on)

void ReleaseControl(Entry& e) {
    SDK::AEnemyBase_C* en = e.controlledEnemy;
    if (Valid(en) && !en->bDead) {
        en->bIsAIDisabled = false;
        if (SDK::UCharacterMovementComponent* cm = en->CharacterMovement; Valid(cm)) {
            const SDK::EMovementMode m =
                e.savedMoveMode == SDK::EMovementMode::MOVE_None ? SDK::EMovementMode::MOVE_Walking : e.savedMoveMode;
            cm->SetMovementMode(m, 0);
        }
    }
    e.controlled = false;
    e.controlledEnemy = nullptr;
}

void TakeControl(Entry& e, SDK::AEnemyBase_C* en) {
    if (e.controlled && e.controlledEnemy != en) ReleaseControl(e);
    SDK::UCharacterMovementComponent* cm = en->CharacterMovement;
    if (!Valid(cm)) cm = nullptr;
    if (!e.controlled) {
        e.controlled = true;
        e.controlledEnemy = en;
        e.savedMoveMode = cm ? cm->MovementMode : SDK::EMovementMode::MOVE_Walking;
        QD_DEBUG(kTag, "follow {:016X} ({}), AI off", e.id, ue::Name(en));
    }
    en->bIsAIDisabled = true;   // re-asserted every frame: the enemy BP may clear it
    if (cm && cm->MovementMode != SDK::EMovementMode::MOVE_None) cm->DisableMovement();
}

// Takeover: this machine becomes the simulator. Align to the freshest data we have (last snapshot from the
// previous owner, else the host's EnemyOwner values), then let the AI run.
void BecomeSimulator(Entry& e, const net::MsgEnemyOwner* hint, bool align, double now) {
    SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
    if (en && !en->bDead && align) {
        bool have = false;
        net::Vec3f pos{};
        float yaw = 0, hp = 0, mx = 0;
        if (!e.snaps.empty() && now - e.snaps.back().t < kAlignFreshSec) {
            const net::EnemyState& st = e.snaps.back().s;
            pos = st.pos;
            yaw = st.yaw;
            hp = st.hp;
            mx = st.maxHp;
            have = true;
            // the hit that caused this hand-over was settled on the host after that snapshot; the host's hint
            // already contains it. Take the lower HP so the hit is not lost.
            if (hint && hint->hp > 0 && hint->maxHp > 0 && mx > 0) hp = std::min(hp, hint->hp / hint->maxHp * mx);
        } else if (hint) {
            pos = hint->pos;
            yaw = hint->yaw;
            hp = hint->hp;
            mx = hint->maxHp;
            have = true;
        }
        if (have) {
            SDK::FHitResult hit{};
            en->K2_SetActorLocationAndRotation(ToF(pos), SDK::FRotator(0.0f, yaw, 0.0f), false, &hit, true);
            if (hp > 0) {
                const float localMax = QueryMaxHp(en);
                if (mx > 0 && localMax > 0 && std::fabs(localMax - mx) > 0.005f * mx) hp = hp / mx * localMax;
                en->HP = hp;
            }
        }
    }
    if (en) {
        if (en->bDead) e.diedSentFor = en;   // it died while we were not simulating it: never report that as a kill
        e.prevStaggered = en->bIsStaggered;
        if (SDK::UCharacterMovementComponent* cm = en->CharacterMovement; Valid(cm)) cm->Velocity = SDK::FVector(0, 0, 0);
    }
    if (en && !en->bDead && now - e.blobAt < kBlobFreshSec) ApplyStoredBlob(e, en, "takeover");
    if (e.controlled) ReleaseControl(e);
    e.snaps.clear();
    e.ownerAliveSince = e.ownerDeadSince = -1;
    e.ownerDiedEvent = false;
}

// Hand-over: drop the old buffer, wait for the new owner's snapshots. If we were simulating, freeze the
// enemy right away (the new owner aligned to our last snapshot, so "stay here" is the right pose meanwhile).
void BecomeFollower(Entry& e, bool wasSimulator) {
    e.snaps.clear();
    e.ownerAliveSince = e.ownerDeadSince = -1;
    e.ownerDiedEvent = false;
    SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
    if (wasSimulator && en && !en->bDead) {
        TakeControl(e, en);
        if (SDK::UCharacterMovementComponent* cm = en->CharacterMovement; Valid(cm)) cm->Velocity = SDK::FVector(0, 0, 0);
    }
}

// Records a new owner locally and switches this machine's role for that enemy. False if nothing changed.
bool SetOwnerLocal(uint64_t id, uint8_t newOwner, const char* reason, const net::MsgEnemyOwner* hint, bool align) {
    auto& s = S();
    if (newOwner >= net::kMaxPlayers) return false;
    const double now = Now();
    OwnerRec& rec = s.owners[id];
    const uint8_t old = rec.slot;
    if (old == newOwner) return false;
    rec.prev = old;
    rec.slot = newOwner;
    rec.changedAt = now;
    LogOwner(id, old, newOwner, reason);
    if (auto it = s.entries.find(id); it != s.entries.end()) {
        Entry& e = it->second;
        e.seenAt = -100;
        if (newOwner == s.localSlot)
            BecomeSimulator(e, hint, align, now);
        else
            BecomeFollower(e, old == s.localSlot);
    }
    return true;
}

void FillOwnerMsg(net::MsgEnemyOwner& m) {
    auto& s = S();
    auto it = s.entries.find(m.enemyId);
    if (it == s.entries.end()) return;
    Entry& e = it->second;
    if (SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr) {
        m.pos = ToN(en->K2_GetActorLocation());
        m.yaw = NormalizeAxis(en->K2_GetActorRotation().Yaw);
        m.hp = std::max(0.0f, en->HP);
        m.maxHp = std::max(0.0f, CachedMaxHp(e, en, Now()));
    } else if (Valid(e.spawner)) {
        m.pos = ToN(e.spawner->K2_GetActorLocation());
    }
    if (!std::isfinite(m.hp)) m.hp = 0;   // the codec rejects NaN/inf
    if (!std::isfinite(m.maxHp)) m.maxHp = 0;
}

// Host: the only place owners are decided. Applies locally, then tells every client.
void HostSetOwner(uint64_t id, uint8_t newOwner, const char* reason, bool align) {
    if (!SetOwnerLocal(id, newOwner, reason, nullptr, align)) return;
    net::MsgEnemyOwner m;
    m.enemyId = id;
    m.ownerSlot = newOwner;
    FillOwnerMsg(m);   // after SetOwnerLocal: a host takeover has just aligned its copy
    coop::Broadcast(net::Channel::Reliable, net::Encode(m));
}

// ------------------------------------------------------------------ ids / map

uint64_t SpawnerId(SDK::AEnemySpawner_C* sp) {
    auto& s = S();
    if (auto it = s.spawnerIds.find(sp); it != s.spawnerIds.end()) return it->second;
    uint64_t id = 0;
    SDK::Params::EnemySpawner_C_GetSaveGUID p{};
    if (Call(sp, "GetSaveGUID", p)) {
        const SDK::FGuid& g = p.Output;
        if (g.A | g.B | g.C | g.D) id = ue::Fnv1a64(std::string_view(reinterpret_cast<const char*>(&g), sizeof(g)));
    }
    if (!id) id = ue::Fnv1a64(ue::PathName(sp));
    if (auto o = s.idOwner.find(id); o != s.idOwner.end() && o->second != sp &&
                                     Valid(static_cast<const SDK::UObject*>(o->second))) {
        const uint64_t alt = ue::Fnv1a64(ue::PathName(sp));
        QD_WARN(kTag, "duplicate spawner GUID id {:016X} ({}), using its path hash {:016X}", id, ue::Name(sp), alt);
        id = alt;
    }
    s.idOwner[id] = sp;
    s.spawnerIds[sp] = id;
    return id;
}

// True when the entry now points at a different enemy actor than before (respawn / first sight).
bool AttachEnemy(Entry& e, SDK::AEnemyBase_C* en) {
    bool changed = false;
    if (e.enemy != en) {
        if (e.controlled) ReleaseControl(e);
        e.enemy = en;
        e.prevStaggered = false;
        e.maxHp = 0;
        e.maxHpAt = -100;
        e.clientMaxAt = -100;
        e.lastAttackIdx = -1;
        if (en->bDead) e.diedSentFor = en;   // already dead when we first saw it: never report it as a fresh kill
        changed = true;
    }
    e.seenEnemy = true;
    return changed;
}

void Rebuild(double now) {
    auto& s = S();
    RefreshClasses();
    static std::vector<SDK::AActor*> spawners, enemies;
    spawners.clear();
    enemies.clear();
    if (s.cls.spawner) ActorsOfClass(s.cls.spawner, spawners);
    if (s.cls.enemyBase) ActorsOfClass(s.cls.enemyBase, enemies);

    for (auto& [id, e] : s.entries) e.seenSpawner = e.seenEnemy = false;
    std::unordered_map<const void*, uint64_t> byEnemy, bySpawner;
    std::vector<uint64_t> newActors;   // host: an owned-by-a-client entry got a new enemy actor (respawn on the host)

    for (SDK::AActor* a : spawners) {
        auto* sp = static_cast<SDK::AEnemySpawner_C*>(a);
        if (!Valid(sp)) continue;
        const uint64_t id = SpawnerId(sp);
        Entry& e = s.entries[id];
        e.id = id;
        e.spawner = sp;
        e.seenSpawner = true;
        bySpawner[sp] = id;
        SDK::AEnemyBase_C* m = sp->CurrentManagedEnemy;
        if (Valid(m) && !IsPreview(m) && !byEnemy.contains(m)) {
            if (AttachEnemy(e, m)) newActors.push_back(id);
            byEnemy[m] = id;
        }
    }
    for (SDK::AActor* a : enemies) {
        auto* en = static_cast<SDK::AEnemyBase_C*>(a);
        if (!Valid(en) || IsPreview(en) || en->bDead || byEnemy.contains(en)) continue;
        uint64_t id = 0;
        if (Valid(en->OwningSpawner)) {
            if (auto it = bySpawner.find(en->OwningSpawner); it != bySpawner.end()) {
                const Entry& owner = s.entries[it->second];
                if (!owner.seenEnemy) id = it->second;   // spawner already holds another enemy -> separate id
            }
        }
        if (!id) id = ue::Fnv1a64(ue::PathName(en));   // placed enemies: path names match across machines
        Entry& e = s.entries[id];
        e.id = id;
        if (AttachEnemy(e, en)) newActors.push_back(id);
        byEnemy[en] = id;
    }

    for (auto it = s.entries.begin(); it != s.entries.end();) {
        Entry& e = it->second;
        if (!e.seenSpawner) e.spawner = nullptr;
        if (!e.seenEnemy) {
            if (e.controlled) ReleaseControl(e);
            e.enemy = nullptr;
        }
        if (!e.spawner && !e.enemy && now - e.seenAt > kEntryStaleSec) {
            it = s.entries.erase(it);
            continue;
        }
        ++it;
    }
    s.byEnemy.swap(byEnemy);

    for (auto it = s.spawnerIds.begin(); it != s.spawnerIds.end();)
        it = Valid(static_cast<const SDK::UObject*>(it->first)) ? std::next(it) : s.spawnerIds.erase(it);
    for (auto it = s.spawnerAllowUntil.begin(); it != s.spawnerAllowUntil.end();)
        it = now < it->second ? std::next(it) : s.spawnerAllowUntil.erase(it);

    if (s.active) {
        bool changed = false;
        for (auto& [id, e] : s.entries)
            if (Valid(e.enemy) && !e.enemy->bDead) changed |= NoteEnemyTier(e.enemy, e.bossBar);
        if (changed) ApplyScaling("new enemy data");
    }

    // Host: a fresh actor on the host (respawn, rest, host reload) restarts that enemy under the host. No alignment:
    // the old owner's last snapshot describes the previous life of the enemy.
    if (s.active && s.isHost)
        for (uint64_t id : newActors)
            if (OwnerOf(id).slot != 0) HostSetOwner(id, 0, "刷新", false);
}

// ------------------------------------------------------------------ players (relevance / nearest)

void BuildCandidates(double now) {
    auto& s = S();
    s.cands.clear();
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (Valid(ch)) s.cands.push_back({s.localSlot, ch->K2_GetActorLocation(), !LocalDead(ch)});
    if (!s.active) return;

    const uint16_t area = LocalAreaId();
    const auto& members = coop::Members();
    for (uint8_t slot = 0; slot < net::kMaxPlayers; ++slot) {
        const coop::Member& m = members[slot];
        if (!m.present || m.isLocal || slot == s.localSlot) continue;
        const RemoteTrack& rt = s.remotes[slot];
        if (!rt.has || now - rt.at > kRemoteStaleSec || rt.area != area) continue;   // same area only
        net::Vec3f feet{};
        float yaw = 0;
        if (!playersync::RemotePose(slot, feet, yaw)) continue;
        s.cands.push_back({slot, SDK::FVector(feet.x, feet.y, feet.z + kFeetToCenter), !rt.dead});
    }
}

bool NearAnyPlayer(const SDK::FVector& p, float range) {
    for (const Cand& c : S().cands)
        if (Dist(p, c.pos) <= range) return true;
    return false;
}

const Cand* CandOf(uint8_t slot) {
    for (const Cand& c : S().cands)
        if (c.slot == slot) return &c;
    return nullptr;
}
bool CandAlive(uint8_t slot) {
    const Cand* c = CandOf(slot);
    return c && c->alive;
}

// ------------------------------------------------------------------ host: ownership evaluation

// Rule 2 input: is the enemy fighting its current owner? Live fields when we simulate it, else the owner's snapshot.
bool Engaged(const Entry& e, SDK::AEnemyBase_C* en, uint8_t owner, double now) {
    if (owner == S().localSlot) return EngagedLive(en);
    return !e.snaps.empty() && now - e.seenAt < kOwnerSilentSec && (e.snaps.back().s.flags & 4) != 0;
}

const Cand* NearestCand(const SDK::FVector& pos, const Entry& e, double now) {
    const Cand* best = nullptr;
    float bestD = 0;
    for (const Cand& c : S().cands) {
        if (!c.alive) continue;
        if (c.slot == e.excludeSlot && now < e.excludeUntil) continue;
        const float d = Dist(pos, c.pos);
        if (!best || d < bestD) {
            best = &c;
            bestD = d;
        }
    }
    return best;
}

void HostEvaluateOwners(double now) {
    auto& s = S();
    std::vector<OwnerChange> changes;

    // Owners that cannot simulate any more: left, silent, or the enemy is dead / gone on the host (the host's
    // spawners are the only authority on death and respawn, so a dead enemy always belongs to the host).
    for (auto& [id, rec] : s.owners) {
        if (rec.slot == 0) continue;
        if (!MemberPresent(rec.slot)) {
            changes.push_back({id, 0, "离开", true});
            continue;
        }
        auto it = s.entries.find(id);
        if (it == s.entries.end()) {
            changes.push_back({id, 0, "消失", false});
            continue;
        }
        Entry& e = it->second;
        if (!Valid(e.enemy) || e.enemy->bDead) {
            changes.push_back({id, 0, "死亡", false});
            continue;
        }
        if (now - rec.changedAt > kOwnerStaleSec && now - e.seenAt > kOwnerStaleSec) {
            // Owners only report enemies within 60 m of some player. Far from everyone: just take it back. Near
            // someone but silent (owner lacks the enemy, is loading, ...): take it back and skip that owner for a while.
            if (!NearAnyPlayer(e.enemy->K2_GetActorLocation(), kRelevantRange)) {
                changes.push_back({id, 0, "远离", true});
            } else {
                e.excludeSlot = rec.slot;
                e.excludeUntil = now + kStaleExcludeSec;
                changes.push_back({id, 0, "失联", true});
            }
        }
    }
    for (const OwnerChange& c : changes) HostSetOwner(c.id, c.slot, c.reason, c.align);
    changes.clear();

    for (auto& [id, e] : s.entries) {
        SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
        if (!en || en->bDead) continue;
        const SDK::FVector pos = en->K2_GetActorLocation();
        if (!NearAnyPlayer(pos, kRelevantRange)) continue;   // only enemies within 60 m of someone take part
        const OwnerRec& rec = OwnerOf(id);
        const uint8_t cur = rec.slot;
        const bool boss = IsBossLike(e, en);
        bool ownerNear = false;
        if (boss && CandAlive(cur))
            for (const Cand& k : s.cands)
                if (k.slot == cur && k.alive && Dist(pos, k.pos) <= kRelevantRange) ownerNear = true;
        if (e.lastHitSlot < net::kMaxPlayers && now - e.lastHitAt < kHitAggroSec && CandAlive(e.lastHitSlot)) {
            // 1. aggro: the last hitter (repeat hits by the current owner change nothing). Bosses: at most every 12 s
            //    while their owner is still there.
            const double minGap = boss && ownerNear ? kBossHitSwitchSec : kHitSwitchMinSec;
            if (e.lastHitSlot != cur && now - rec.changedAt >= minGap)
                changes.push_back({id, e.lastHitSlot, "命中", true});
        } else if (boss && ownerNear) {
            // a boss stays with its owner while that player is alive and within 60 m
        } else if (CandAlive(cur) && Engaged(e, en, cur, now)) {
            // 2. chasing / attacking its owner: keep
        } else if (now - rec.changedAt >= kOwnerSwitchSec) {
            // 3. nearest living player in the host's area (4. at most one change per 3 s)
            const Cand* c = NearestCand(pos, e, now);
            if (c && c->slot != cur) {
                const float dNew = Dist(pos, c->pos);
                const Cand* owner = nullptr;
                for (const Cand& k : s.cands)
                    if (k.slot == cur && k.alive) owner = &k;
                const float dCur = owner ? Dist(pos, owner->pos) : 1e9f;
                const bool clearlyCloser = dNew <= kNearAssignRange && dNew + kNearMarginCm <= dCur && dNew <= dCur * kNearMarginRatio;
                if (clearlyCloser || !owner) changes.push_back({id, c->slot, "最近", true});
            }
        }
    }
    for (const OwnerChange& c : changes) HostSetOwner(c.id, c.slot, c.reason, c.align);
}

// Host: a newcomer gets every non-host owner (it already knows "everything else is the host's").
void HostSendOwnerTable(uint8_t slot) {
    auto& s = S();
    int sent = 0;
    for (const auto& [id, rec] : s.owners) {
        if (rec.slot == 0) continue;
        net::MsgEnemyOwner m;
        m.enemyId = id;
        m.ownerSlot = rec.slot;
        FillOwnerMsg(m);
        coop::SendToSlot(slot, net::Channel::Reliable, net::Encode(m));
        ++sent;
    }
    QD_INFO(kTag, "owner table -> slot {}: {} enemies owned by clients", slot, sent);
}

// ------------------------------------------------------------------ simulator: events / snapshots

// Host: to every client except `except` (the original sender of a relayed message).
void RelayExcept(uint8_t except, net::Channel ch, const std::vector<uint8_t>& bytes) {
    const auto& mem = coop::Members();
    for (uint8_t slot = 1; slot < net::kMaxPlayers; ++slot)
        if (slot != except && mem[slot].present && !mem[slot].isLocal) coop::SendToSlot(slot, ch, bytes);
}

// Simulator only. Host: to all clients; client: to the host, which relays it.
void SendEvent(uint64_t id, net::EnemyEventType type, int32_t arg, uint32_t seed = 0) {
    net::MsgEnemyEvent ev;
    ev.enemyId = id;
    ev.type = type;
    ev.arg = arg;
    ev.seed = seed;
    ev.ownerSlot = S().localSlot;
    coop::Broadcast(net::Channel::Reliable, net::Encode(ev));
    LogEvent(std::format("发送 {} {:04X} arg={}", EventName(type), ShortId(id), arg));
}

// has anything a follower would notice changed since the state we sent last?
bool StateChanged(const net::EnemyState& a, const net::EnemyState& b) {
    if (a.flags != b.flags || a.stateIndex != b.stateIndex || a.subState != b.subState || a.targetSlot != b.targetSlot)
        return true;
    if (std::fabs(a.hp - b.hp) > 0.01f || std::fabs(a.maxHp - b.maxHp) > 0.01f) return true;
    if (std::fabs(NormalizeAxis(a.yaw - b.yaw)) > 1.0f) return true;
    const float dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y, dz = a.pos.z - b.pos.z;
    return dx * dx + dy * dy + dz * dz > 4.0f;   // 2 cm
}

// Every machine, for the enemies it owns: Died / Staggered edges + EnemyStates (20 Hz when changing, 2 Hz when idle).
void SimTick(double now, float dt) {
    auto& s = S();
    s.stateAccum += dt;
    if (s.stateAccum < kStateInterval) return;
    s.stateAccum = std::fmod(s.stateAccum, kStateInterval);

    net::MsgEnemyStates msg;
    msg.seq = ++s.seq;
    msg.ownerSlot = s.localSlot;
    int live = 0;
    for (auto& [id, e] : s.entries) {
        if (!IsMine(id)) {
            e.relevant = false;
            continue;
        }
        SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
        if (en) {   // edge events (the Died hook usually got there first)
            if (en->bDead && e.diedSentFor != en) {
                e.diedSentFor = en;
                SendEvent(id, net::EnemyEventType::Died, 0);
            }
            const bool stag = en->bIsStaggered;
            if (stag && !e.prevStaggered && !en->bDead) SendEvent(id, net::EnemyEventType::Staggered, 0);
            e.prevStaggered = stag;
        }

        // The host also reports dead / not-spawned enemies (its spawners decide life and death); a client simulator
        // only ever reports live ones (its Died event covers the death).
        SDK::FVector pos;
        if (en && (s.isHost || !en->bDead)) pos = en->K2_GetActorLocation();
        else if (s.isHost && Valid(e.spawner)) pos = e.spawner->K2_GetActorLocation();
        else {
            e.relevant = false;
            continue;
        }
        e.relevant = NearAnyPlayer(pos, kRelevantRange);
        if (!e.relevant) continue;

        net::EnemyState st;
        st.enemyId = id;
        st.pos = ToN(pos);
        if (en) {
            st.yaw = NormalizeAxis(en->K2_GetActorRotation().Yaw);
            st.hp = std::max(0.0f, en->HP);
            st.maxHp = CachedMaxHp(e, en, now);
            if (!std::isfinite(st.hp)) st.hp = 0;
            if (!std::isfinite(st.maxHp)) st.maxHp = 0;
            st.stateIndex = static_cast<uint8_t>(std::clamp(en->CurrentStateIndex, 0, 255));
            st.subState = static_cast<uint8_t>(std::clamp(en->CurrentSubState, 0, 255));
            const bool engaged = EngagedLive(en);
            uint8_t f = 0;
            if (en->bDead) f |= 1;
            if (en->bIsStaggered) f |= 2;
            if (engaged) f |= 4;
            if (IsBoss(e, en)) f |= 8;
            if (en->InTrapMode) f |= 16;
            if (e.wokenFor == en) f |= 32;
            if (e.bossBar) f |= 64;
            st.flags = f;
            // on the simulator, player 0 (= us) is the target
            st.targetSlot = engaged && !en->bDead ? s.localSlot : static_cast<uint8_t>(0xFF);
            if (!en->bDead) ++live;
            if (!en->bDead && now - e.blobSentAt >= kBlobInterval && IsBossLike(e, en)) SendBlob(e, en, now);
            if (!en->bDead) ReportMontage(e, en);
        } else {
            st.flags = 1;   // spawner without a live enemy: dead / not spawned on the host
        }
        if (e.sentHave) {
            const net::EnemyState& o = e.sentState;
            const bool hard = o.flags != st.flags || o.targetSlot != st.targetSlot || o.stateIndex != st.stateIndex ||
                              o.subState != st.subState || std::fabs(o.hp - st.hp) > 0.01f || std::fabs(o.maxHp - st.maxHp) > 0.01f;
            if (!hard) {
                double gap = 0.0;   // engaged and moving: every tick
                if (!StateChanged(o, st)) gap = (st.flags & 1) ? kDeadStateInterval : kIdleStateInterval;
                else if ((st.flags & 4) == 0) gap = kWanderStateInterval;
                if (now - e.sentAt < gap) continue;
            }
        }
#if QD_DEV
        {   // dev: why are states being sent? (10 s summary)
            static int cnt[6] = {};
            static double last = 0;
            int r = 5;   // periodic
            if (!e.sentHave) r = 0;
            else if (e.sentState.flags != st.flags || e.sentState.targetSlot != st.targetSlot || e.sentState.stateIndex != st.stateIndex || e.sentState.subState != st.subState) r = 1;
            else if (std::fabs(e.sentState.hp - st.hp) > 0.01f || std::fabs(e.sentState.maxHp - st.maxHp) > 0.01f) r = 2;
            else if (std::fabs(NormalizeAxis(e.sentState.yaw - st.yaw)) > 1.0f) r = 3;
            else if (StateChanged(e.sentState, st)) r = 4;
            ++cnt[r];
            if (now - last > 10.0) {
                QD_INFO(kTag, "sent states / 10 s: first {} flags {} hp {} yaw {} pos {} periodic {}", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
                for (int& c : cnt) c = 0;
                last = now;
            }
        }
#endif
        e.sentState = st;
        e.sentHave = true;
        e.sentAt = now;
        msg.enemies.push_back(st);
    }
    s.simulated = live;
    if (msg.enemies.empty()) return;
    for (const auto& chunk : net::EncodeSplit(msg)) coop::Broadcast(net::Channel::Unreliable, chunk);
}

void BroadcastHpScale(int count) {
    auto& s = S();
    const auto& h = config::Get().hpScale;
    const float extra = static_cast<float>(std::max(0, count - 1));
    net::MsgHpScale m;
    m.normal = 1.0f + h.normal * extra;
    m.elite = 1.0f + h.elite * extra;
    m.boss = 1.0f + h.boss * extra;
    m.playerCount = static_cast<uint8_t>(std::clamp(count, 1, net::kMaxPlayers));
    s.hpScale = m;
    coop::Broadcast(net::Channel::Reliable, net::Encode(m));
    QD_INFO(kTag, "HpScale -> {} players: normal x{:.2f}, elite x{:.2f}, boss x{:.2f}", count, m.normal, m.elite, m.boss);
}

uint32_t NextSeed() {
    static std::mt19937 rng{std::random_device{}()};
    return static_cast<uint32_t>(rng());
}

// True if the enemy is one of the classes whose attacks go through kCustomAttacks (computed once per instance).
bool HasCustomAttacks(Entry& e, SDK::AEnemyBase_C* en) {
    if (!en) return e.customAttacks;
    if (e.customCheckedFor == en) return e.customAttacks;
    e.customCheckedFor = en;
    e.customAttacks = false;
    for (const auto& ca : kCustomAttacks) {
        const std::string_view path(ca.path);
        const auto colon = path.find(':');
        if (colon == std::string_view::npos) continue;
        SDK::UClass* cls = ue::FindClass(path.substr(0, colon));
        if (cls && en->IsA(cls)) {
            e.customAttacks = true;
            break;
        }
    }
    return e.customAttacks;
}

// ---- simulator hooks

bool OnStartAttackPre(hooks::CallInfo& ci) {
    auto& s = S();
    if (!s.active || !ci.object || !ci.params) return false;
    auto bi = s.byEnemy.find(ci.object);
    if (bi == s.byEnemy.end() || !IsMine(bi->second)) return false;
    auto ei = s.entries.find(bi->second);
    if (ei == s.entries.end() || !ei->second.relevant) return false;
    Entry& e = ei->second;
    auto* en = static_cast<SDK::AEnemyBase_C*>(ci.object);
    if (s.customDepth > 0 || HasCustomAttacks(e, en)) return false;   // CustomAttack covers it
    const int32_t idx = static_cast<SDK::Params::FS_EnemyCharacterBase_BP_StartAttackState*>(ci.params)->AttackState;
    const double now = Now();
    if (idx == e.lastAttackIdx && now - e.lastAttackAt < kAttackDedupSec) return false;
    e.lastAttackIdx = idx;
    e.lastAttackAt = now;
    // The attack's Blueprint runs right after this hook: seed the game's rand() so a follower replaying it (elite / boss,
    // see ReplayAttack) rolls the same random choices.
    const uint32_t seed = NextSeed();
    ue::SeedGameRand(seed);
    SendEvent(e.id, net::EnemyEventType::AttackStart, idx, seed);
    return false;
}

// Wakes this machine's copy (follower side, or the owner when another player's copy rose first). Once per instance.
void WakeLocal(Entry& e, SDK::AEnemyBase_C* en, bool damaged, const char* why) {
    if (!en || en->bDead || e.wokenFor == en) return;
    e.wokenFor = en;
    // No InTrapMode check: some bosses wait in their own dormant state with InTrapMode false (the Whaleking
    // meditates) and still start through ActivateTrap. wokenFor already tells us this copy has not been started.
    auto& s = S();
    SDK::Params::EnemyBase_C_ActivateTrap p{};
    p.Damaged = damaged;
    s.wakingLocal = true;
    Call(en, "ActivateTrap", p);
    s.wakingLocal = false;
    QD_INFO(kTag, "{:016X} ({}) woken: {}", e.id, ue::Name(en), why);
}

// Any machine: its copy of an enemy left trap mode. Our own WakeLocal is not echoed.
void OnActivateTrapPost(hooks::CallInfo& ci) {
    auto& s = S();
    if (!ci.object) return;
    auto bi = s.byEnemy.find(ci.object);
    if (bi == s.byEnemy.end()) return;
    auto ei = s.entries.find(bi->second);
    if (ei == s.entries.end()) return;
    Entry& e = ei->second;
    auto* en = static_cast<SDK::AEnemyBase_C*>(ci.object);
    e.wokenFor = en;
    if (!s.active || s.wakingLocal || en->bDead) return;
    const bool damaged = ci.params && static_cast<SDK::Params::EnemyBase_C_ActivateTrap*>(ci.params)->Damaged;
    QD_INFO(kTag, "{:016X} ({}) rose here, telling the others", e.id, ue::Name(en));
    SendEvent(e.id, net::EnemyEventType::Woke, damaged ? 1 : 0);
}

bool OnCustomAttackPre(hooks::CallInfo& ci, int index) {
    auto& s = S();
    const bool outermost = s.customDepth++ == 0;
    if (!s.active || !ci.object || !outermost || s.replayingCustom) return false;
    auto bi = s.byEnemy.find(ci.object);
    if (bi == s.byEnemy.end()) return false;
    auto ei = s.entries.find(bi->second);
    if (ei == s.entries.end()) return false;
    Entry& e = ei->second;
    e.customAttacks = true;
    if (!IsMine(bi->second) || !e.relevant) return false;
    SendEvent(e.id, net::EnemyEventType::CustomAttack, index);
    return false;
}

// Any machine: the local player started an interaction with an enemy (talk-to-start bosses). The others get a notice.
void OnEnemyInteractPost(hooks::CallInfo& ci) {
    auto& s = S();
    if (!s.active || !ci.object) return;
    auto bi = s.byEnemy.find(ci.object);
    if (bi == s.byEnemy.end()) return;
    auto ei = s.entries.find(bi->second);
    if (ei == s.entries.end()) return;
    Entry& e = ei->second;
    const double now = Now();
    if (now - e.interactSentAt < kInteractNoticeSec) return;
    e.interactSentAt = now;
    QD_INFO(kTag, "{:016X} ({}): local interaction, telling the others", e.id, ue::Name(ci.object));
    SendEvent(e.id, net::EnemyEventType::Interacted, 0);
}

void OnCustomAttackPost(hooks::CallInfo&) {
    auto& s = S();
    if (s.customDepth > 0) --s.customDepth;
}

void OnManagedDiedPost(hooks::CallInfo& ci) {
    auto& s = S();
    if (!s.active || !ci.params) return;
    auto* p = static_cast<SDK::Params::EnemySpawner_C_ManagedEnemyDied*>(ci.params);
    const void* en = p->EnemyThatDied;
    uint64_t id = 0;
    if (auto it = s.byEnemy.find(en); it != s.byEnemy.end()) id = it->second;
    else if (auto it2 = s.spawnerIds.find(ci.object); it2 != s.spawnerIds.end()) id = it2->second;
    if (!id || !IsMine(id)) return;   // followers killed their copy because of the owner's Died: nothing to report
    auto ei = s.entries.find(id);
    if (ei == s.entries.end() || ei->second.diedSentFor == en) return;
    ei->second.diedSentFor = en;
    SendEvent(id, net::EnemyEventType::Died, p->NoReward ? 1 : 0);   // arg 1 = no rewards (despawn-style death)
}

void OnBossBarPost(hooks::CallInfo& ci, bool on) {
    auto& s = S();
    if (!s.active || !ci.params) return;
    // ActivateBossHPBar / DeactivateBossHPBar share the layout {AEnemyBase_C* Enemy; UEnemyData_C* Group;}
    SDK::AEnemyBase_C* en = static_cast<SDK::Params::FS_PlayerChar_C_ActivateBossHPBar*>(ci.params)->Enemy;
    if (!Valid(en)) return;
    auto bi = s.byEnemy.find(en);
    if (bi == s.byEnemy.end()) return;
    auto ei = s.entries.find(bi->second);
    if (ei == s.entries.end()) return;
    HookScope scope;
    const uint64_t id = ei->first;
    if (on && !ei->second.bossBar) {
        ei->second.bossBar = true;
        if (NoteEnemyTier(en, true)) ApplyScaling("boss bar");   // calls GetMaxHP (ProcessEvent)
    }
    if (IsMine(id)) SendEvent(id, on ? net::EnemyEventType::BossBarOn : net::EnemyEventType::BossBarOff, 0);
}

// ------------------------------------------------------------------ damage: always settled on the owner

std::string DamageKey(const net::MsgDamageRequest& m) {
    std::string key;
    for (const auto& t : m.damageTags) key += t + ';';
    key += '|';
    for (const auto& d : m.dataTags) key += d.tag + ';';
    return key;
}

std::unique_ptr<DamageTemplate> BuildTemplate(const net::MsgDamageRequest& m) {
    auto t = std::make_unique<DamageTemplate>();

    std::vector<SDK::FGameplayTag> tags;
    for (const auto& name : m.damageTags) {
        SDK::FGameplayTag tag{};
        tag.TagName = ue::MakeName(name);
        if (!tag.TagName.IsNone()) tags.push_back(tag);
    }
    if (!tags.empty()) {
        // Engine-built container so ParentTags is filled too (HasTag with parent matching).
        SDK::UObject* lib = SDK::UBlueprintGameplayTagLibrary::GetDefaultObj();
        SDK::Params::BlueprintGameplayTagLibrary_MakeGameplayTagContainerFromArray p{};
        const auto n = static_cast<int32_t>(tags.size());
        p.GameplayTags = UC::TArray<SDK::FGameplayTag>(tags.data(), n, n);   // read-only input
        if (!Call(lib, "MakeGameplayTagContainerFromArray", p)) return nullptr;
        t->set.DamageTags = p.ReturnValue;
    }

    SDK::UObject* utils = SDK::UFB_DamageAttributeUtilities::GetDefaultObj();
    SDK::FFB_DamageAttributeSet cur = t->set;
    for (const auto& dt : m.dataTags) {
        const SDK::FName n = ue::MakeName(dt.tag);
        t->dataNames.push_back(n);
        if (n.IsNone()) continue;
        SDK::Params::FB_DamageAttributeUtilities_AddDataTagToDamage p{};
        p.Damage = cur;
        p.Tag.TagName = n;
        p.Value = dt.value;
        if (!Call(utils, "AddDataTagToDamage", p)) return nullptr;
        cur = p.ReturnValue;   // engine-owned; intermediates leak once per distinct tag signature (bounded)
    }
    t->set = cur;
    return t;
}

SDK::FFB_DamageAttributeSet* PrepareDamage(const net::MsgDamageRequest& m) {
    auto& cache = S().damageCache;
    const std::string key = DamageKey(m);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto built = BuildTemplate(m);
        if (!built) {
            QD_WARN(kTag, "could not build damage set for tags '{}'", key);
            return nullptr;
        }
        it = cache.emplace(key, std::move(built)).first;
        QD_DEBUG(kTag, "damage template #{}: '{}'", cache.size(), key);
        if (cache.size() == 256) QD_WARN(kTag, "256 distinct damage tag signatures cached (each keeps a few engine allocations)");
    }
    DamageTemplate& t = *it->second;
    t.set.DamageAmount = m.damage;
    std::memcpy(&t.set.SourceGUID, m.sourceGuid.data(), sizeof(t.set.SourceGUID));
    auto& map = t.set.DataTags;
    for (int32_t i = 0; i < map.NumAllocated(); ++i) {
        if (!map.IsValidIndex(i)) continue;
        auto& pr = map[i];
        for (size_t k = 0; k < t.dataNames.size() && k < m.dataTags.size(); ++k) {
            if (pr.First.TagName == t.dataNames[k]) {
                pr.Second = m.dataTags[k].value;
                break;
            }
        }
    }
    return &t.set;
}

// The owner settles a remote player's hit with the game's own entry point. Applier = the local player character: on
// the simulating machine the enemy should aggro its player 0 anyway, and ownership follows the attacker within 0.5 s.
void ApplyRemoteDamage(Entry& e, const net::MsgDamageRequest& m) {
    auto& s = S();
    SDK::AEnemyBase_C* en = e.enemy;
    if (!Valid(en) || en->bDead || en->HP <= 0.0f) return;
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!Valid(ch) || s.applyingRemote) return;

    SDK::FFB_DamageAttributeSet* dmg = PrepareDamage(m);
    if (!dmg) return;

    auto& p = *s.tadParams;
    const SDK::FVector loc = en->K2_GetActorLocation();
    p.Hit = SDK::FHitResult{};
    p.Hit.Location.X = p.Hit.ImpactPoint.X = loc.X;
    p.Hit.Location.Y = p.Hit.ImpactPoint.Y = loc.Y;
    p.Hit.Location.Z = p.Hit.ImpactPoint.Z = loc.Z;
    p.DamageToApply = *dmg;   // shallow: the BP only reads it (BlueprintReadOnly Parm)
    p.Applier = ch;
    p.HitLocation = loc;
    p.HitLocationIsValid = false;
    p.ReturnValue = false;
    const float before = en->HP;
    s.applyingRemote = true;   // our TryApplyDamage pre hook must not treat this as a local hit
    Call(en, "TryApplyDamage", p);
    s.applyingRemote = false;
    QD_DEBUG(kTag, "slot {} hit {:016X} for {:.1f}: HP {:.1f} -> {:.1f}", m.attackerSlot, m.enemyId, m.damage, before,
             Valid(en) ? en->HP : -1.0f);
    LogEvent(std::format("伤害 P{} → {:04X} {:.0f}", m.attackerSlot + 1, ShortId(m.enemyId), m.damage));
}

void OnDamageRequest(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.active) return;
    net::MsgDamageRequest m;
    if (!net::Decode(data, size, m)) return;
    if (!std::isfinite(m.damage) || m.damage < 0.0f || m.damage > kMaxDamage) {
        QD_WARN(kTag, "slot {} damage request rejected: damage {}", fromSlot, m.damage);
        return;
    }
    if (m.damageTags.size() > kMaxTags || m.dataTags.size() > kMaxTags) return;
    for (const auto& dt : m.dataTags)
        if (!std::isfinite(dt.value)) return;
    const double now = Now();
    auto it = s.entries.find(m.enemyId);

    if (s.isHost) {
        if (fromSlot == 0 || fromSlot >= net::kMaxPlayers) return;
        // a hit a client sent back because it stopped owning the enemy while the hit was on its way (the hit
        // itself usually caused that hand-over). The attacker is whoever the host recorded the first time.
        const bool forwarded = (m.flags & net::kDamageForwarded) != 0;
        if (forwarded) {
            if (m.attackerSlot >= net::kMaxPlayers) return;
        } else {
            m.attackerSlot = fromSlot;   // never trust the slot a client claims
            if (it != s.entries.end()) {   // aggro input for the ownership evaluation (rule 1)
                it->second.lastHitSlot = fromSlot;
                it->second.lastHitAt = now;
            }
        }
        if (m.flags & net::kDamageHitNotifyOnly) return;   // the owner already settled it locally
        const uint8_t owner = OwnerOf(m.enemyId).slot;
        if (owner != 0 && MemberPresent(owner)) {   // relay to the owner, with the original attacker filled in
            coop::SendToSlot(owner, net::Channel::Reliable, net::Encode(m));
            LogEvent(std::format("转发 P{} → {:04X} 给 P{}", fromSlot + 1, ShortId(m.enemyId), owner + 1));
            return;
        }
        if (it == s.entries.end()) {
            QD_DEBUG(kTag, "slot {} damage request for unknown enemy {:016X}", fromSlot, m.enemyId);
            return;
        }
        ApplyRemoteDamage(it->second, m);
        return;
    }

    // client: a request the host relayed to us because we own the enemy (aggro is the host's business)
    if (m.flags & net::kDamageHitNotifyOnly) return;
    if (it == s.entries.end()) return;
    if (!IsMine(m.enemyId)) {
        // The enemy changed owner while this hit was on its way to us. It used to be dropped — in play, the first
        // hit of a player on an enemy somebody else owned often did nothing. Hand it back to the host once.
        if ((m.flags & net::kDamageForwarded) == 0) {
            m.flags |= net::kDamageForwarded;
            coop::SendToHost(net::Channel::Reliable, net::Encode(m));
            QD_INFO(kTag, "relayed hit on {:016X} sent back to the host: owner is now P{}", m.enemyId, OwnerOf(m.enemyId).slot + 1);
        }
        return;
    }
    ApplyRemoteDamage(it->second, m);
}

// ------------------------------------------------------------------ local hits (TryApplyDamage hooks)

bool IsShielded(const SDK::UObject* en) {
    for (const ShieldFrame& f : S().shield)
        if (f.active && f.enemy == en) return true;
    return false;
}

bool IsLocalPlayerSource(SDK::AActor* applier) {
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!ch) return false;
    SDK::AActor* a = applier;
    for (int depth = 0; depth < 6 && a && Valid(a); ++depth) {
        if (a == ch) return true;
        if (a->Instigator && static_cast<SDK::AActor*>(a->Instigator) == ch) return true;
        a = a->Owner;
    }
    return false;
}

// Follower: forward the hit to the enemy's owner (host: directly; client: via the host, which relays it).
void SendDamageRequest(Entry& e, uint8_t owner, const SDK::FFB_DamageAttributeSet& d) {
    auto& s = S();
    net::MsgDamageRequest m;
    m.enemyId = e.id;
    m.damage = d.DamageAmount;
    const auto& tags = d.DamageTags.GameplayTags;
    for (int32_t i = 0; i < tags.Num() && m.damageTags.size() < kMaxTags; ++i)
        m.damageTags.push_back(tags[i].TagName.GetRawString());
    const auto& map = d.DataTags;
    for (int32_t i = 0; i < map.NumAllocated() && m.dataTags.size() < kMaxTags; ++i) {
        if (!map.IsValidIndex(i)) continue;
        const auto& pr = map[i];
        m.dataTags.push_back({pr.First.TagName.GetRawString(), pr.Second});
    }
    std::memcpy(m.sourceGuid.data(), &d.SourceGUID, sizeof(d.SourceGUID));
    m.attackerSlot = s.localSlot;
    m.flags = 0;
    if (!std::isfinite(m.damage)) return;
    if (s.isHost) {
        e.lastHitSlot = s.localSlot;
        e.lastHitAt = Now();
        coop::SendToSlot(owner, net::Channel::Reliable, net::Encode(m));
    } else {
        coop::SendToHost(net::Channel::Reliable, net::Encode(m));
    }
}

// Simulator: the local player hit an enemy we own — the game settles it normally; tell the host who hit (aggro).
void NoteLocalHit(Entry& e, float damage) {
    auto& s = S();
    const double now = Now();
    if (s.isHost) {
        e.lastHitSlot = s.localSlot;
        e.lastHitAt = now;
        return;
    }
    if (now - e.lastNotifyAt < kHitNotifySec) return;
    e.lastNotifyAt = now;
    net::MsgDamageRequest m;
    m.enemyId = e.id;
    m.damage = std::isfinite(damage) ? std::clamp(damage, 0.0f, kMaxDamage) : 0.0f;
    m.attackerSlot = s.localSlot;
    m.flags = net::kDamageHitNotifyOnly;
    coop::SendToHost(net::Channel::Reliable, net::Encode(m));
}

// TryApplyDamage on an enemy (any machine):
//  - we own it: settle normally; a local hit is reported to the host as aggro input;
//  - we follow it (AI off): forward the original DamageToApply to its owner, then let the BP run (hit FX, stagger)
//    against a huge temporary HP so nothing is lost locally; post restores HP.
// Frames form a stack because BP overrides that call Parent:TryApplyDamage fire the hook twice (nested).
bool OnTryApplyDamagePre(hooks::CallInfo& ci) {
    auto& s = S();
    HookScope scope;   // SendDamageRequest -> GetRawString is not ProcessEvent, but LocalCharacter() may be
    ShieldFrame f;
    f.enemy = ci.object;
    if (s.active && !s.applyingRemote && ci.object && ci.params && !IsShielded(ci.object)) {
        if (auto bi = s.byEnemy.find(ci.object); bi != s.byEnemy.end()) {
            auto ei = s.entries.find(bi->second);
            auto* en = static_cast<SDK::AEnemyBase_C*>(ci.object);
            if (ei != s.entries.end() && !en->bDead) {
                Entry& e = ei->second;
                auto* p = static_cast<SDK::Params::EnemyBase_C_TryApplyDamage*>(ci.params);
                const uint8_t owner = OwnerOf(e.id).slot;
                if (owner == s.localSlot) {
                    if (IsLocalPlayerSource(p->Applier)) NoteLocalHit(e, p->DamageToApply.DamageAmount);
                } else if (e.controlled && e.controlledEnemy == en) {
                    if (IsLocalPlayerSource(p->Applier)) SendDamageRequest(e, owner, p->DamageToApply);
                    f.active = true;
                    f.savedHp = en->HP;
                    en->HP = kShieldHp;
                }
            }
        }
    }
    if (s.shield.size() > 64) {
        QD_WARN(kTag, "TryApplyDamage shield stack overflow, clearing");
        s.shield.clear();
    }
    s.shield.push_back(f);
    return false;
}

void OnTryApplyDamagePost(hooks::CallInfo& ci) {
    auto& s = S();
    while (!s.shield.empty()) {
        const ShieldFrame f = s.shield.back();
        s.shield.pop_back();
        if (f.active && Valid(f.enemy)) {
            auto* en = const_cast<SDK::AEnemyBase_C*>(static_cast<const SDK::AEnemyBase_C*>(f.enemy));
            en->HP = f.savedHp;
        }
        if (f.enemy == ci.object) break;
    }
}

// ------------------------------------------------------------------ spawners

// Client: the host decides when spawners (re)spawn. Host: never blocks; a rest / reset on the
// host takes back the enemies of that spawner (handled in Tick, see HostTakeBackResets).
bool OnSpawnerPre(hooks::CallInfo& ci) {
    auto& s = S();
    if (!s.active || !ci.object) return false;
    if (s.isHost) {
        if (ci.function && s.hostSpawnerResets.size() < 1024) {
            const std::string fn = ue::Name(ci.function);
            if (fn == "PlayerRested" || fn == "Reset") s.hostSpawnerResets.push_back(ci.object);
        }
        return false;
    }
    if (auto it = s.spawnerAllowUntil.find(ci.object); it != s.spawnerAllowUntil.end() && Now() < it->second) return false;
    if (s.blockedLogs < 20) {
        ++s.blockedLogs;
        QD_DEBUG(kTag, "client: blocked {} on {}", ci.function ? ue::Name(ci.function) : std::string("?"), ue::Name(ci.object));
    }
    return true;   // the host decides when this spawner (re)spawns
}

void HostTakeBackResets() {
    auto& s = S();
    if (s.hostSpawnerResets.empty()) return;
    std::vector<const void*> resets;
    resets.swap(s.hostSpawnerResets);
    std::sort(resets.begin(), resets.end());
    resets.erase(std::unique(resets.begin(), resets.end()), resets.end());
    for (const void* sp : resets) {
        auto it = s.spawnerIds.find(sp);
        if (it == s.spawnerIds.end()) continue;
        if (OwnerOf(it->second).slot != 0) HostSetOwner(it->second, 0, "重置", false);   // keep the host's reset pose
    }
}

void ForceRespawn(Entry& e) {
    auto& s = S();
    if (!Valid(e.spawner)) return;
    s.spawnerAllowUntil[e.spawner] = Now() + kSpawnerAllowSec;
    uint8_t noParams[16]{};
    if (Call(e.spawner, "Force Respawn", noParams))
        LogEvent(std::format("对齐：刷出 {:04X}", ShortId(e.id)));
}

// ------------------------------------------------------------------ follower: events / snapshots

void PlayAttack(SDK::AEnemyBase_C* en, int32_t idx) {
    SDK::Params::FS_EnemyCharacterBase_GetAttackMontage_Base p{};
    p.Attack = idx;
    Call(en, "GetAttackMontage_Base", p);
    SDK::UAnimMontage* montage = Valid(p.ReturnValue) ? p.ReturnValue : nullptr;
    if (!montage && Valid(en->FallbackAttackAnim)) montage = en->FallbackAttackAnim;
    if (!montage) {
        QD_DEBUG(kTag, "no attack montage {} on {}", idx, ue::Name(en));
        return;
    }
    en->SelectedAttackConfig = idx;   // the montage's hitbox notifies look up damage/status by the selected attack
    en->PlayAnimMontage(montage, 1.0f, SDK::FName());
}

uint8_t TierOf(SDK::AEnemyBase_C* en) {
    uint8_t t = ClassTier(en);
    if (SDK::UEnemyData_C* d = en->Data; Valid(d)) {
        if (d->Boss) t = kBoss;
        if (auto it = S().datas.find(d); it != S().datas.end()) t = std::max(t, it->second.tier);
    }
    return t;
}

// Follower side of AttackStart. Regular enemies: the attack montage only (their hitboxes are montage notifies, which
// is enough and was verified in play). Elites and bosses: their own Blueprint attack logic — overridden AttemptAttack,
// attack actions, BP-driven hitboxes, projectiles — so it runs BP_StartAttackState on our copy, with the game's rand()
// seeded like the owner's. If that does not start a montage (the BP declined, e.g. a state check), fall back to the
// plain montage.
void ReplayAttack(Entry& e, SDK::AEnemyBase_C* en, int32_t idx, uint32_t seed) {
    auto& s = S();
    if (TierOf(en) < kElite && !e.bossBar) {
        PlayAttack(en, idx);
        return;
    }
    WakeLocal(e, en, false, "attack");
    SDK::Params::FS_EnemyCharacterBase_BP_StartAttackState p{};
    p.AttackState = idx;
    ue::SeedGameRand(seed);
    s.replayingCustom = true;
    const bool called = Call(en, "BP_StartAttackState", p);
    s.replayingCustom = false;
    if (!called || !Valid(en->GetCurrentMontage())) {
        QD_DEBUG(kTag, "{:016X}: BP attack replay {} started no montage, playing the plain one", e.id, idx);
        PlayAttack(en, idx);
    }
}

void CallBossBar(SDK::AEnemyBase_C* en, bool on) {
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!IsFsPlayer(ch)) return;
    SDK::Params::EnemyBase_C_Enemy_GetBossInfo bi{};
    Call(en, "Enemy_GetBossInfo", bi);
    if (on) {
        SDK::Params::FS_PlayerChar_C_ActivateBossHPBar p{};
        p.Enemy = en;
        p.Group = bi.Group__optional_;
        Call(ch, "ActivateBossHPBar", p);
    } else {
        SDK::Params::FS_PlayerChar_C_DeactivateBossHPBar p{};
        p.Enemy = en;
        p.Group = bi.Group__optional_;
        Call(ch, "DeactivateBossHPBar", p);
    }
}

// Snapshots are accepted only from the enemy's current owner.
void AcceptStates(const net::MsgEnemyStates& m) {
    auto& s = S();
    SeqTrack& sq = s.seqs[m.ownerSlot];
    if (sq.has && static_cast<int16_t>(static_cast<uint16_t>(m.seq - sq.last)) < 0) return;   // strictly older
    sq.has = true;
    sq.last = m.seq;
    const double now = Now();
    int rejected = 0;
    for (const net::EnemyState& st : m.enemies) {
        if (OwnerOf(st.enemyId).slot != m.ownerSlot) {
            ++rejected;
            continue;
        }
        Entry& e = s.entries[st.enemyId];
        e.id = st.enemyId;
        e.seenAt = now;
        if (st.flags & 1) {
            e.ownerAliveSince = -1;
            if (e.ownerDeadSince < 0) e.ownerDeadSince = now;
        } else {
            e.ownerDeadSince = -1;
            e.ownerDiedEvent = false;
            if (e.ownerAliveSince < 0) e.ownerAliveSince = now;
        }
        // idle enemies only report every 0.5 s. Without an anchor, the first snapshot of a movement would be
        // interpolated against one that is half a second old and the enemy would seem to start early and slowly.
        // Repeat the old state just before the new one so the motion starts when it really did.
        if (!e.snaps.empty() && now - e.snaps.back().t > kGapAnchorSec) {
            const net::EnemyState held = e.snaps.back().s;
            e.snaps.push_back({now - kStateInterval, held});
        }
        e.snaps.push_back({now, st});
        while (e.snaps.size() > kSnapMax) e.snaps.pop_front();
    }
    if (rejected) QD_DEBUG(kTag, "{} snapshot(s) from P{} ignored (not the owner)", rejected, m.ownerSlot + 1);
}

void OnEnemyStates(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.active) return;
    net::MsgEnemyStates m;
    if (!net::Decode(data, size, m)) return;
    if (s.isHost) {
        if (fromSlot == 0 || fromSlot >= net::kMaxPlayers) return;
        if (m.ownerSlot != fromSlot) {   // never trust the slot a client claims
            m.ownerSlot = fromSlot;
            RelayExcept(fromSlot, net::Channel::Unreliable, net::Encode(m));
        } else {
            RelayExcept(fromSlot, net::Channel::Unreliable, std::vector<uint8_t>(data, data + size));
        }
    } else if (m.ownerSlot == s.localSlot) {
        return;   // our own snapshot echoed back (the host never does this, but cheap to guard)
    }
    AcceptStates(m);
}

// Events: only from the current owner; a Died from the previous owner is still honoured for kOwnerGraceSec (the kill
// raced the hand-over).
bool AcceptEvent(const net::MsgEnemyEvent& ev) {
    const OwnerRec& rec = OwnerOf(ev.enemyId);
    if (ev.ownerSlot == rec.slot) return true;
    if (ev.type == net::EnemyEventType::Woke) return true;   // sticky: whoever saw it rise first
    if (ev.type == net::EnemyEventType::Interacted) return true;
    return ev.type == net::EnemyEventType::Died && ev.ownerSlot == rec.prev && Now() - rec.changedAt < kOwnerGraceSec;
}

void ApplyEvent(const net::MsgEnemyEvent& ev) {
    auto& s = S();
    LogEvent(std::format("收到 {} {:04X} arg={} (P{})", EventName(ev.type), ShortId(ev.enemyId), ev.arg, ev.ownerSlot + 1));
    auto it = s.entries.find(ev.enemyId);
    if (it == s.entries.end()) return;
    Entry& e = it->second;
    SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
    const double now = Now();
    switch (ev.type) {
        case net::EnemyEventType::AttackStart:
            if (en && !en->bDead && !IsMine(ev.enemyId) && !HasCustomAttacks(e, en)) {
                TakeControl(e, en);
                ReplayAttack(e, en, ev.arg, ev.seed);
            }
            break;
        case net::EnemyEventType::Woke:
            WakeLocal(e, en, ev.arg != 0, "remote Woke");
            break;
        case net::EnemyEventType::Interacted: {
            std::string who;
            if (ev.ownerSlot < net::kMaxPlayers) who = coop::Members()[ev.ownerSlot].name;
            if (who.empty()) who = "P" + std::to_string(ev.ownerSlot + 1);
            ui::Toast(ui::TrF("toast.boss_talk", who), ui::ToastKind::Info, 6.0f);
            break;
        }
        case net::EnemyEventType::CustomAttack:
            if (en && !en->bDead && !IsMine(ev.enemyId) && ev.arg >= 0 && ev.arg < kCustomAttackCount) {
                WakeLocal(e, en, false, "custom attack");
                TakeControl(e, en);
                e.customAttacks = true;
                alignas(16) uint8_t parms[64] = {};   // DoNormalAttack / DoBigAttack take no parameters
                s.replayingCustom = true;
                Call(en, kCustomAttacks[ev.arg].name, parms);
                s.replayingCustom = false;
            }
            break;
        case net::EnemyEventType::Staggered:
            if (en && !en->bDead && Valid(en->StaggerAnim)) en->PlayAnimMontage(en->StaggerAnim, 1.0f, SDK::FName());
            break;
        case net::EnemyEventType::Died:
            // Every non-owner — the host included — kills its own copy and rolls its own rewards. On the host this
            // Kill runs the spawner's normal ManagedEnemyDied flow, so respawning stays the host's decision.
            e.ownerDiedEvent = true;
            e.ownerAliveSince = -1;
            if (e.ownerDeadSince < 0) e.ownerDeadSince = now;
            if (en && !en->bDead) {
                e.diedSentFor = en;   // set first: Kill -> ManagedEnemyDied must not echo a Died of our own
                KillEnemy(en, ev.arg != 0, false);
                e.nextReconcileAt = now + kKillRetrySec + 1.0;
            }
            break;
        case net::EnemyEventType::BossBarOn:
        case net::EnemyEventType::BossBarOff:
            if (en) {
                if (ev.type == net::EnemyEventType::BossBarOn && !e.bossBar) {
                    e.bossBar = true;
                    if (NoteEnemyTier(en, true)) ApplyScaling("boss bar (remote)");
                }
                CallBossBar(en, ev.type == net::EnemyEventType::BossBarOn);
            }
            break;
    }
}

void OnEnemyEvent(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.active) return;
    net::MsgEnemyEvent ev;
    if (!net::Decode(data, size, ev)) return;
    if (s.isHost) {
        if (fromSlot == 0 || fromSlot >= net::kMaxPlayers) return;
        ev.ownerSlot = fromSlot;
    } else if (ev.ownerSlot == s.localSlot) {
        return;
    }
    if (!AcceptEvent(ev)) {
        QD_DEBUG(kTag, "event {} for {:016X} from P{} ignored (owner P{})", static_cast<int>(ev.type), ev.enemyId,
                 ev.ownerSlot + 1, OwnerOf(ev.enemyId).slot + 1);
        return;
    }
    if (s.isHost) RelayExcept(fromSlot, net::Channel::Reliable, net::Encode(ev));
    ApplyEvent(ev);
}

void OnEnemyMontage(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.active) return;
    net::MsgEnemyMontage m;
    if (!net::Decode(data, size, m)) return;
    if (s.isHost) {
        if (fromSlot == 0 || fromSlot >= net::kMaxPlayers) return;
        m.ownerSlot = fromSlot;
        RelayExcept(fromSlot, net::Channel::Reliable, net::Encode(m));
    } else if (m.ownerSlot == s.localSlot) {
        return;
    }
    if (OwnerOf(m.enemyId).slot != m.ownerSlot) return;
    auto it = s.entries.find(m.enemyId);
    if (it == s.entries.end()) return;
    Entry& e = it->second;
    e.mirrorPath = m.path;
    e.mirrorAt = Now();
    SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
    if (!en || en->bDead || IsMine(m.enemyId)) return;
    TakeControl(e, en);
    MirrorMontage(e, en, m.path);
}

void OnEnemyBlob(uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.active) return;
    net::MsgEnemyBlob m;
    if (!net::Decode(data, size, m)) return;
    if (s.isHost) {
        if (fromSlot == 0 || fromSlot >= net::kMaxPlayers) return;
        m.ownerSlot = fromSlot;
        RelayExcept(fromSlot, net::Channel::Unreliable, net::Encode(m));
    } else if (m.ownerSlot == s.localSlot) {
        return;
    }
    if (OwnerOf(m.enemyId).slot != m.ownerSlot) return;   // only the current owner's state counts
    auto it = s.entries.find(m.enemyId);
    if (it == s.entries.end()) return;
    Entry& e = it->second;
    e.blob = std::move(m.data);
    e.blobLayout = m.layout;
    e.blobAt = Now();
    // Keep our copy's resting state in step too (it replays the boss's attacks with its own Blueprint), but never
    // while it is in the middle of one.
    SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
    if (en && !en->bDead && !IsMine(m.enemyId) && !Valid(en->GetCurrentMontage())) ApplyStoredBlob(e, en, nullptr);
}

void OnEnemyOwner(uint8_t, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.active || s.isHost) return;   // only the host assigns owners
    net::MsgEnemyOwner m;
    if (!net::Decode(data, size, m)) return;
    SetOwnerLocal(m.enemyId, m.ownerSlot, "房主指定", &m, true);
}

void OnHpScale(uint8_t, const uint8_t* data, size_t size) {
    auto& s = S();
    if (s.isHost) return;
    net::MsgHpScale m;
    if (!net::Decode(data, size, m)) return;
    s.hpScale = m;
    s.rescalePending = true;   // we may simulate enemies too -> same multipliers as the host (next Tick)
    QD_INFO(kTag, "host HP scale: {} players, normal x{:.2f}, elite x{:.2f}, boss x{:.2f}", m.playerCount, m.normal,
            m.elite, m.boss);
}

void OnPlayerState(uint8_t, const uint8_t* data, size_t size) {
    net::MsgPlayerState m;
    if (!net::Decode(data, size, m) || m.slot >= net::kMaxPlayers) return;
    RemoteTrack& rt = S().remotes[m.slot];
    rt.has = true;
    rt.dead = m.actionId == 3;
    rt.area = m.areaId;
    rt.at = Now();
}

void Interpolate(const std::deque<Snap>& b, double t, net::Vec3f& pos, float& yaw, net::Vec3f& vel) {
    vel = {};
    if (t <= b.front().t) {
        pos = b.front().s.pos;
        yaw = b.front().s.yaw;
        return;
    }
    if (t >= b.back().t) {
        pos = b.back().s.pos;
        yaw = b.back().s.yaw;
        if (b.size() >= 2 && t - b.back().t < 0.2) {
            const Snap& a = b[b.size() - 2];
            const double span = b.back().t - a.t;
            if (span > 1e-3 && span < 0.5 && NDist(a.s.pos, b.back().s.pos) < kSnapDistance) {
                const float inv = static_cast<float>(1.0 / span);
                vel = {(b.back().s.pos.x - a.s.pos.x) * inv, (b.back().s.pos.y - a.s.pos.y) * inv,
                       (b.back().s.pos.z - a.s.pos.z) * inv};
            }
        }
        return;
    }
    for (size_t i = b.size() - 1; i > 0; --i) {
        const Snap& a = b[i - 1];
        const Snap& c = b[i];
        if (t < a.t) continue;
        const double span = c.t - a.t;
        const float alpha = span > 1e-6 ? static_cast<float>((t - a.t) / span) : 1.0f;
        if (NDist(a.s.pos, c.s.pos) > kSnapDistance) {
            pos = alpha < 0.5f ? a.s.pos : c.s.pos;
            yaw = alpha < 0.5f ? a.s.yaw : c.s.yaw;
            return;
        }
        pos = {a.s.pos.x + (c.s.pos.x - a.s.pos.x) * alpha, a.s.pos.y + (c.s.pos.y - a.s.pos.y) * alpha,
               a.s.pos.z + (c.s.pos.z - a.s.pos.z) * alpha};
        yaw = LerpAngle(a.s.yaw, c.s.yaw, alpha);
        if (span > 1e-3) {
            const float inv = static_cast<float>(1.0 / span);
            vel = {(c.s.pos.x - a.s.pos.x) * inv, (c.s.pos.y - a.s.pos.y) * inv, (c.s.pos.z - a.s.pos.z) * inv};
        }
        return;
    }
    pos = b.front().s.pos;
    yaw = b.front().s.yaw;
}

void DriveEnemy(Entry& e, SDK::AEnemyBase_C* en, uint8_t owner, double now) {
    auto& s = S();
    net::Vec3f pos{}, vel{};
    float yaw = 0;
    // wandering enemies arrive at 5 Hz, fighting ones at 20 Hz. Render 1.5 snapshot intervals in the past, eased
    // so the delay does not jump when an enemy starts or stops fighting.
    double want = kInterpDelay;
    if (e.snaps.size() >= 2) {
        const double gap = e.snaps.back().t - e.snaps[e.snaps.size() - 2].t;
        if (gap > 0.0 && gap < 0.5) want = std::clamp(gap * 1.5, kInterpDelay, 0.35);
    }
    e.followDelay += (want - e.followDelay) * 0.1;
    Interpolate(e.snaps, now - e.followDelay, pos, yaw, vel);
    SDK::FHitResult hit{};
    en->K2_SetActorLocationAndRotation(ToF(pos), SDK::FRotator(0.0f, yaw, 0.0f), false, &hit, true);
    if (SDK::UCharacterMovementComponent* cm = en->CharacterMovement; Valid(cm)) cm->Velocity = ToF(vel);   // anim BP speed
    const net::EnemyState& last = e.snaps.back().s;
    if (!s.isHost && owner == 0) ClientMatchMaxHp(e, en, last.maxHp, now);
    if (!IsShielded(en)) {
        // Same HP fraction as the owner (the two ends may disagree slightly on the maximum, e.g. tier).
        float hp = last.hp;
        const float localMax = CachedMaxHp(e, en, now);
        if (last.maxHp > 0 && localMax > 0 && std::fabs(localMax - last.maxHp) > 0.005f * last.maxHp)
            hp = last.hp / last.maxHp * localMax;
        const float before = en->HP;
        en->HP = hp;
        if (before < kShieldHp * 0.5f && std::fabs(before - hp) > 0.5f) NotifyHpWidgets(en, std::max(0.0f, before - hp));
    }
}

// the native boss state machine. FS_EnemyCharacterBase keeps CurrentStateIndex (the boss "form" / state: Queen,
// Josami Lord, Necronomicon) and CurrentSubState (the phase: SetCurrentSubState(Phase) -> OnSubStateChanged(NewPhase)).
// Both were already in every snapshot but never applied, so a follower's copy stayed in phase 0: no phase visuals, and
// its attack replays ran phase-0 logic. Applied through the game's own setters so the Blueprint events fire. Two equal
// snapshots in a row are required (a value seen once may be a transition frame).
void FollowBossState(Entry& e, SDK::AEnemyBase_C* en, const net::EnemyState& prev, const net::EnemyState& last) {
    struct IntParm { int32_t v; };
    if (prev.stateIndex == last.stateIndex && en->CurrentStateIndex != last.stateIndex && e.stateSet != last.stateIndex) {
        e.stateSet = last.stateIndex;   // one attempt per value: a Blueprint that refuses it is not asked every frame
        IntParm p{last.stateIndex};
        Call(en, "SetCurrentStateIndex", p);
        QD_INFO(kTag, "{:016X} ({}): state index -> {} (owner's)", e.id, ue::Name(en), last.stateIndex);
    }
    if (prev.subState == last.subState && en->CurrentSubState != last.subState && e.subSet != last.subState) {
        e.subSet = last.subState;
        IntParm p{last.subState};
        Call(en, "SetCurrentSubState", p);
        QD_INFO(kTag, "{:016X} ({}): phase -> {} (owner's)", e.id, ue::Name(en), last.subState);
    }
    if (en->CurrentStateIndex == last.stateIndex) e.stateSet = -1;
    if (en->CurrentSubState == last.subState) e.subSet = -1;
}

// Every machine, for the enemies it does NOT own (the host included).
void FollowerTick(double now) {
    auto& s = S();
    int driven = 0, controlled = 0;
    for (auto& [id, e] : s.entries) {
        const uint8_t owner = OwnerOf(id).slot;
        if (owner == s.localSlot) {   // simulator: the AI must run
            if (e.controlled) ReleaseControl(e);
            continue;
        }
        if (e.controlled) ++controlled;
        if (e.snaps.empty()) continue;
        const net::EnemyState& last = e.snaps.back().s;
        const bool fresh = now - e.seenAt < kOwnerSilentSec;
        const bool ownerDead = (last.flags & 1) != 0;
        SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
        const bool localAlive = en && !en->bDead;

        // life/death reconcile. Force Respawn only on clients: the host's spawners are the authority.
        if (fresh && now >= e.nextReconcileAt) {
            if (!s.isHost && !ownerDead && !localAlive && Valid(e.spawner) && e.ownerAliveSince >= 0 &&
                now - e.ownerAliveSince >= kAliveConfirmSec) {
                ForceRespawn(e);
                e.nextReconcileAt = now + kRespawnRetrySec;
            } else if (ownerDead && localAlive && e.ownerDeadSince >= 0 && now - e.ownerDeadSince >= kDeadGraceSec) {
                e.diedSentFor = en;
                KillEnemy(en, true, true);   // dead on the owner without a Died event (e.g. before we joined)
                e.nextReconcileAt = now + kKillRetrySec;
                LogEvent(std::format("对齐：移除 {:04X}", ShortId(id)));
            }
        }
        if (!localAlive || !fresh) continue;
        TakeControl(e, en);
        if (ownerDead) continue;
        if ((last.flags & 16) == 0 && en->InTrapMode && e.wokenFor != en) WakeLocal(e, en, false, "owner's copy is up");
        // the Woke / BossBarOn events are lost when our copy did not exist yet (spawned later, joined later);
        // the snapshot flags repeat them.
        if ((last.flags & 32) && e.wokenFor != en) WakeLocal(e, en, false, "owner's copy was started");
        if ((last.flags & 64) && !e.bossBar) {
            e.bossBar = true;
            if (NoteEnemyTier(en, true)) ApplyScaling("boss bar (snapshot)");
            CallBossBar(en, true);
            QD_INFO(kTag, "{:016X} ({}): boss bar shown (owner has it)", e.id, ue::Name(en));
        }
        if (e.snaps.size() >= 2 && IsBossLike(e, en)) FollowBossState(e, en, e.snaps[e.snaps.size() - 2].s, last);
        DriveEnemy(e, en, owner, now);
        ++driven;
    }
    s.followed = driven;
    s.controlledCount = controlled;
}

// ------------------------------------------------------------------ session / world transitions

void BeginSession() {
    auto& s = S();
    s.seq = 0;
    s.seqs = {};
    s.owners.clear();   // everything starts under the host
    s.pendingOwnerSync.clear();
    s.hostSpawnerResets.clear();
    s.rescalePending = false;
    s.lastPlayerCount = 0;
    s.stateAccum = 0;
    s.ownerAccum = 0;
    for (auto& [id, e] : s.entries) {
        e.snaps.clear();
        e.seenAt = -100;
        e.ownerAliveSince = e.ownerDeadSince = -1;
        e.ownerDiedEvent = false;
        e.nextReconcileAt = 0;
        e.lastHitSlot = 0xFF;
        e.lastHitAt = -100;
        e.excludeSlot = 0xFF;
        e.excludeUntil = 0;
        if (Valid(e.enemy) && e.enemy->bDead) e.diedSentFor = e.enemy;
    }
    QD_INFO(kTag, "session started as {} (slot {})", s.isHost ? "host" : "client", s.localSlot);
    if (!hooks::ScriptHooksComplete()) {
        QD_WARN(kTag, "unknown game build: Blueprint hooks are incomplete, enemy sync will miss events");
        ui::Toast(ui::Tr("toast.game_updated"), ui::ToastKind::Warning, 12.0f);
    }
}

void EndSession(bool wasHost) {
    auto& s = S();
    int released = 0;
    for (auto& [id, e] : s.entries) {
        if (e.controlled) {
            ReleaseControl(e);
            ++released;
        }
        e.snaps.clear();
        e.ownerAliveSince = e.ownerDeadSince = -1;
        e.ownerDiedEvent = false;
    }
    s.owners.clear();
    s.spawnerAllowUntil.clear();
    s.pendingOwnerSync.clear();
    s.hostSpawnerResets.clear();
    s.hpScale = net::MsgHpScale{};
    s.rescalePending = false;
    s.simulated = s.followed = s.controlledCount = 0;
    ApplyScaling("session end");   // MultFor() is 1 now (active == false) -> originals back, HP % kept
    RestoreAllData();              // also undoes ClientMatchMaxHp adjustments
    QD_INFO(kTag, "session ended ({}): released {} enemies", wasHost ? "host" : "client", released);
}

// Session / role / slot bookkeeping. Runs at the top of Tick AND before every network handler: the first messages of
// a session (Welcome + owner table) can arrive in the same frame, before our ticker has seen the session start.
void RefreshSession() {
    auto& s = S();
    const bool inSession = coop::InSession();
    const bool host = coop::IsHost();
    const auto slot = coop::LocalSlot();
    const uint8_t localSlot = slot ? *slot : 0;
    if (inSession != s.active || (inSession && host != s.isHost)) {
        const bool wasActive = s.active, wasHost = s.isHost;
        if (wasActive) {
            s.active = false;
            EndSession(wasHost);
        }
        s.isHost = host;
        s.localSlot = localSlot;
        if (inSession) {
            s.active = true;
            BeginSession();
        }
    }
    s.isHost = host;
    s.localSlot = localSlot;
}

// Network entry point for our handlers: refresh the session view, or queue the message while a hook callback runs.
void Receive(RawHandler fn, uint8_t from, const uint8_t* data, size_t size) {
    auto& s = S();
    if (!s.inited || !data || size == 0) return;
    if (s.hookDepth > 0) {
        if (s.inbox.size() < kInboxMax) s.inbox.push_back({fn, from, std::vector<uint8_t>(data, data + size)});
        else QD_WARN(kTag, "inbox full, message dropped");
        return;
    }
    HookScope scope;
    RefreshSession();
    fn(from, data, size);
}

void DrainInbox() {
    auto& s = S();
    if (s.inbox.empty()) return;
    std::vector<Deferred> batch;
    batch.swap(s.inbox);
    for (const Deferred& d : batch) Receive(d.fn, d.from, d.bytes.data(), d.bytes.size());
}

void PublishStats() {
    auto& s = S();
    std::lock_guard lk(g_statMutex);
    g_stats.inSession = s.active;
    g_stats.host = s.isHost;
    g_stats.localSlot = s.localSlot;
    g_stats.registered = static_cast<int>(s.entries.size());
    g_stats.simulated = s.simulated;
    g_stats.followed = s.followed;
    g_stats.controlled = s.controlledCount;
    if (s.active && s.hpScale.playerCount > 0) {
        g_stats.playerCount = s.hpScale.playerCount;
        g_stats.multNormal = s.hpScale.normal;
        g_stats.multElite = s.hpScale.elite;
        g_stats.multBoss = s.hpScale.boss;
    } else {
        g_stats.playerCount = 1;
        g_stats.multNormal = g_stats.multElite = g_stats.multBoss = 1.0f;
    }
}

void Tick(float dt) {
    auto& s = S();
    if (s.hookDepth > 0) return;   // re-entered from inside one of our hook callbacks: skip this frame
    const double now = Now();

    SDK::UWorld* world = ue::World();
    if (world != s.world) {
        // The old world's actors go away with it: forget them without touching them (EnemyData assets survive).
        // Ownership (s.owners) is kept: it is session state, not world state.
        s.entries.clear();
        s.byEnemy.clear();
        s.spawnerIds.clear();
        s.idOwner.clear();
        s.spawnerAllowUntil.clear();
        s.hostSpawnerResets.clear();
        s.shield.clear();
        s.cands.clear();
        s.world = world;
        s.rebuildAccum = kRebuildInterval;
        QD_INFO(kTag, "world changed -> {}; enemy map rebuilt from scratch", world ? ue::Name(world) : std::string("<none>"));
    }

    DrainInbox();
    HookScope tickScope;   // a nested Pump during our ProcessEvent calls must not run handlers into our iterations
    RefreshSession();

    if (world) {
        s.rebuildAccum += dt;
        if (s.rebuildAccum >= (s.active ? kRebuildInterval : kIdleRebuildInterval)) {
            s.rebuildAccum = 0;
            Rebuild(now);
        }
        if (s.active) BuildCandidates(now);

        if (s.active) {
            if (s.rescalePending) {
                s.rescalePending = false;
                ApplyScaling("host HpScale");
            }
            if (s.isHost) {
                const int count = coop::PlayerCount();
                if (count != s.lastPlayerCount || s.resendHpScale) {
                    const bool changed = count != s.lastPlayerCount;
                    s.lastPlayerCount = count;
                    s.resendHpScale = false;
                    BroadcastHpScale(count);
                    if (changed) ApplyScaling("player count");
                }
                if (!s.pendingOwnerSync.empty()) {
                    std::vector<uint8_t> slots;
                    slots.swap(s.pendingOwnerSync);
                    for (uint8_t slot : slots)
                        if (MemberPresent(slot)) HostSendOwnerTable(slot);
                }
                HostTakeBackResets();
                s.ownerAccum += dt;
                if (s.ownerAccum >= kOwnerEvalInterval) {
                    s.ownerAccum = 0;
                    HostEvaluateOwners(now);
                }
            }
            SimTick(now, dt);
            FollowerTick(now);
        } else {
            s.simulated = s.followed = s.controlledCount = 0;
        }
    }

    s.statsAccum += dt;
    if (s.statsAccum >= kStatsInterval) {
        s.statsAccum = 0;
        PublishStats();
    }
}

// ------------------------------------------------------------------ debug tab (render thread: cached data only)

#if QD_DEV
void DrawDebugTab() {
    const EnemySyncStats st = GetEnemySyncStats();
    if (!st.inSession)
        ImGui::TextUnformatted("状态：未联机");
    else
        ImGui::Text("状态：%s（P%d）· 每只敌人由它在打的玩家模拟", st.host ? "房主" : "客机", st.localSlot + 1);
    ImGui::Text("已登记敌人 %d · 本机模拟 %d · 本机跟随 %d · AI 已关 %d", st.registered, st.simulated, st.followed,
                st.controlled);
    ImGui::Text("HP 倍率（%d 人）：普通 ×%.2f · 精英 ×%.2f · Boss ×%.2f", st.playerCount, st.multNormal, st.multElite,
                st.multBoss);
    ImGui::SeparatorText("最近 5 次换主（旧主 → 新主 原因）");
    if (st.recentOwners.empty()) ImGui::TextDisabled("（暂无）");
    for (const auto& line : st.recentOwners) ImGui::TextUnformatted(line.c_str());
    ImGui::SeparatorText("最近 5 条敌人事件");
    if (st.recentEvents.empty()) ImGui::TextDisabled("（暂无）");
    for (const auto& line : st.recentEvents) ImGui::TextUnformatted(line.c_str());
}
#endif   // QD_DEV (the debug page is not part of the release)

}  // namespace

// ------------------------------------------------------------------ public

bool RemotePlayerInfo(uint8_t slot, bool& dead, bool& sameArea) {
    if (slot >= net::kMaxPlayers) return false;
    const RemoteTrack& rt = S().remotes[slot];
    if (!rt.has || Now() - rt.at > kRemoteStaleSec) return false;
    dead = rt.dead;
    sameArea = rt.area == LocalAreaId();
    return true;
}

uint16_t LocalAreaId() {
    auto& s = S();
    SDK::UWorld* w = ue::World();
    if (w != s.areaWorld) {
        s.areaWorld = w;
        s.areaId = w ? static_cast<uint16_t>(ue::Fnv1a64(ue::Name(w)) & 0xFFFF) : 0;
    }
    return s.areaId;
}

void ActorsOfClass(SDK::UClass* cls, std::vector<SDK::AActor*>& out) {
    out.clear();
    SDK::UWorld* world = ue::World();
    if (!world || !Valid(cls)) return;
    // GetAllActorsOfClass does OutActors.Reset() + Add(): with spare capacity it never reallocates, so the engine
    // writes straight into this buffer and nothing engine-allocated is left behind per call.
    static SDK::AActor** buffer = static_cast<SDK::AActor**>(std::malloc(sizeof(SDK::AActor*) * kActorBufferCap));
    if (!buffer) return;
    SDK::UObject* lib = SDK::UGameplayStatics::GetDefaultObj();
    static SDK::UFunction* fn = nullptr;
    if (!Valid(fn)) fn = ue::FindFunction(lib, "GetAllActorsOfClass");
    if (!fn) return;
    SDK::Params::GameplayStatics_GetAllActorsOfClass p{};
    p.WorldContextObject = world;
    p.ActorClass = cls;
    p.OutActors = UC::TArray<SDK::AActor*>(buffer, 0, kActorBufferCap);
    ue::CallFunction(lib, fn, &p);
    if (p.OutActors.GetDataPtr() != buffer) {
        QD_ERROR(kTag, "GetAllActorsOfClass reallocated the output buffer ({} actors) — results dropped", p.OutActors.Num());
        return;
    }
    const int n = std::clamp(p.OutActors.Num(), 0, kActorBufferCap);
    out.assign(buffer, buffer + n);
}

EnemySyncStats GetEnemySyncStats() {
    std::lock_guard lk(g_statMutex);
    EnemySyncStats copy = g_stats;
    copy.recentEvents.assign(g_events.begin(), g_events.end());
    copy.recentOwners.assign(g_owners.begin(), g_owners.end());
    return copy;
}

void EnemySyncInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    s.startTime = Now();
    s.tadParams = std::make_unique<SDK::Params::EnemyBase_C_TryApplyDamage>();

    coop::On(net::MsgType::EnemyStates, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnEnemyStates, f, d, n); });
    coop::On(net::MsgType::EnemyEvent, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnEnemyEvent, f, d, n); });
    coop::On(net::MsgType::EnemyOwner, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnEnemyOwner, f, d, n); });
    coop::On(net::MsgType::EnemyBlob, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnEnemyBlob, f, d, n); });
    coop::On(net::MsgType::EnemyMontage, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnEnemyMontage, f, d, n); });
    coop::On(net::MsgType::DamageRequest, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnDamageRequest, f, d, n); });
    coop::On(net::MsgType::HpScale, [](uint8_t f, const uint8_t* d, size_t n) { Receive(OnHpScale, f, d, n); });
    coop::On(net::MsgType::PlayerState, OnPlayerState);
    coop::AddMemberListener([](const coop::Member& m, bool joined) {
        auto& st = S();
        if (m.slot >= net::kMaxPlayers) return;
        st.remotes[m.slot] = RemoteTrack{};
        st.seqs[m.slot] = SeqTrack{};   // a rejoining client restarts its EnemyStates seq at 1
        if (joined && coop::IsHost()) {
            st.resendHpScale = true;                    // newcomer gets the current HpScale
            st.pendingOwnerSync.push_back(m.slot);      // ... and every client-owned enemy (sent after the Welcome)
        }
        // left: the host's next ownership evaluation takes that slot's enemies back ("离开")
    });

    auto& ids = s.hookIds;
    // simulator (whoever owns the enemy)
    ids.push_back(hooks::AddPre(kFnStartAttackState, OnStartAttackPre));
    ids.push_back(hooks::AddPost(kFnActivateTrap, OnActivateTrapPost));
    for (const char* fn : kInteractFns) ids.push_back(hooks::AddPost(fn, OnEnemyInteractPost));
    for (int i = 0; i < kCustomAttackCount; ++i) {
        ids.push_back(hooks::AddPre(kCustomAttacks[i].path, [i](hooks::CallInfo& ci) { return OnCustomAttackPre(ci, i); }));
        ids.push_back(hooks::AddPost(kCustomAttacks[i].path, OnCustomAttackPost));
    }
    ids.push_back(hooks::AddPost(kFnManagedDied, OnManagedDiedPost));
    ids.push_back(hooks::AddPost(kFnBossBarOn, [](hooks::CallInfo& ci) { OnBossBarPost(ci, true); }));
    ids.push_back(hooks::AddPost(kFnBossBarOff, [](hooks::CallInfo& ci) { OnBossBarPost(ci, false); }));
    // both roles: local hits (simulator: aggro notify; follower: shield + forward to the owner)
    ids.push_back(hooks::AddPre(kFnTryApplyDamage, OnTryApplyDamagePre));
    ids.push_back(hooks::AddPost(kFnTryApplyDamage, OnTryApplyDamagePost));
    // spawners: client blocks, host notes rests/resets
    for (const char* fn : kSpawnerBlocked) ids.push_back(hooks::AddPre(fn, OnSpawnerPre));

    s.ticker = gamethread::AddTicker("enemysync", [](float dt) { Tick(dt); });

#if QD_DEV
    ui::AddMenuTab("enemies", "敌人同步", DrawDebugTab, 92);   // ~ console tab (dev flavour only)
#endif
    QD_INFO(kTag, "initialized ({} hooks, per-enemy ownership, protocol v{})", ids.size(), net::kProtocolVersion);
}

void EnemySyncShutdown() {
    auto& s = S();
    if (!s.inited) return;
    for (hooks::HookId id : s.hookIds) hooks::Remove(id);
    s.hookIds.clear();
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    if (s.active) {
        const bool wasHost = s.isHost;
        s.active = false;
        EndSession(wasHost);
    }
    RestoreAllData();
    s.inbox.clear();
    s.inited = false;
}

#if QD_DEV
// ------------------------------------------------------------------ test tooling (dev flavour only)
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool DebugMatch(SDK::AEnemyBase_C* en, const std::string& filterLower) {
    if (filterLower.empty()) return true;
    return Lower(ue::ClassName(en)).find(filterLower) != std::string::npos ||
           Lower(ue::Name(en)).find(filterLower) != std::string::npos;
}

Entry* DebugFind(const std::string& filter, bool aliveOnly = true) {
    auto& s = S();
    const std::string f = Lower(filter);
    SDK::ACharacter* ch = ue::LocalCharacter();
    const SDK::FVector me = ch ? ch->K2_GetActorLocation() : SDK::FVector();
    Entry* best = nullptr;
    float bestD = 1e30f;
    for (auto& [id, e] : s.entries) {
        SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
        if (!en || (aliveOnly && en->bDead) || !DebugMatch(en, f)) continue;
        const float d = Dist(me, en->K2_GetActorLocation());
        if (d < bestD) {
            bestD = d;
            best = &e;
        }
    }
    if (!best) QD_WARN("edbg", "no enemy matches '{}' ({} entries)", filter, s.entries.size());
    return best;
}

void DebugDescribe(Entry& e) {
    auto& s = S();
    SDK::AEnemyBase_C* en = e.enemy;
    const SDK::FVector p = en->K2_GetActorLocation();
    SDK::UAnimMontage* m = en->GetCurrentMontage();
    std::string blob;
    if (const BlobLayout* l = BlobLayoutFor(en)) {
        std::vector<uint8_t> b;
        CaptureBlob(en, *l, b);
        uint32_t h = 2166136261u;
        for (uint8_t v : b) h = (h ^ v) * 16777619u;
        blob = std::format("{:08X}", h);
    }
    QD_INFO("edbg", "E {:016X} {} owner=P{} mine={} hp={:.0f}/{:.0f} state={} sub={} trap={} dead={} ctrl={} aiOff={} boss={} "
                    "bar={} tier={} pos=({:.0f},{:.0f},{:.0f}) montage={} bpstate={}",
            e.id, ue::ClassName(en), OwnerOf(e.id).slot + 1, IsMine(e.id) ? 1 : 0, en->HP, QueryMaxHp(en),
            en->CurrentStateIndex, en->CurrentSubState, en->InTrapMode ? 1 : 0, en->bDead ? 1 : 0, e.controlled ? 1 : 0,
            en->bIsAIDisabled ? 1 : 0, IsBossLike(e, en) ? 1 : 0, e.bossBar ? 1 : 0, TierOf(en), p.X, p.Y, p.Z,
            Valid(m) ? ue::Name(m) : std::string("-"), blob.empty() ? std::string("-") : blob);
    (void)s;
}

}  // namespace

void DebugEnemyList(const std::string& filter) {
    auto& s = S();
    const std::string f = Lower(filter);
    int n = 0;
    for (auto& [id, e] : s.entries) {
        SDK::AEnemyBase_C* en = Valid(e.enemy) ? e.enemy : nullptr;
        if (!en || !DebugMatch(en, f)) continue;
        DebugDescribe(e);
        ++n;
    }
    QD_INFO("edbg", "list '{}': {} of {} entries (session {}, slot {})", filter, n, s.entries.size(), s.active ? 1 : 0, s.localSlot);
}

bool DebugEnemyGoto(const std::string& filter, float dist) {
    Entry* e = DebugFind(filter, false);
    SDK::ACharacter* ch = ue::LocalCharacter();
    SDK::APlayerController* pc = ue::LocalController();
    if (!e || !ch || !pc) return false;
    const SDK::FVector p = e->enemy->K2_GetActorLocation();
    const SDK::FVector me = ch->K2_GetActorLocation();
    float dx = me.X - p.X, dy = me.Y - p.Y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) { dx = 1; dy = 0; len = 1; }
    // Only stand where there is ground: try 12 directions around the enemy (the one towards us first), trace down from
    // above the enemy, and accept a hit within 6 m of the enemy's height. The first version put players in the sea
    // next to the Whaleking's platform.
    const float base = std::atan2(dy, dx);
    SDK::FVector at{};
    bool found = false;
    for (int i = 0; i < 12 && !found; ++i) {
        const float a = base + static_cast<float>(i) * 3.14159265f / 6.0f;
        const SDK::FVector top(p.X + std::cos(a) * dist, p.Y + std::sin(a) * dist, p.Z + 500.0f);
        const SDK::FVector bottom(top.X, top.Y, p.Z - 1500.0f);
        SDK::FHitResult hit{};
        SDK::TArray<SDK::AActor*> ignore;
        const bool blocked = SDK::UKismetSystemLibrary::LineTraceSingle(ch, top, bottom, SDK::ETraceTypeQuery::TraceTypeQuery1, false,
                                                                        ignore, SDK::EDrawDebugTrace::None, &hit, true,
                                                                        SDK::FLinearColor(), SDK::FLinearColor(), 0.0f);
        if (!blocked || !hit.bBlockingHit) continue;
        const float gz = hit.ImpactPoint.Z;
        if (std::fabs(gz - p.Z) > 600.0f) continue;
        at = SDK::FVector(top.X, top.Y, gz + 110.0f);
        found = true;
    }
    if (!found) {
        QD_WARN("edbg", "goto {}: no ground within {:.0f} cm around it, not moving", ue::ClassName(e->enemy), dist);
        return false;
    }
    ch->K2_SetActorLocation(at, false, nullptr, true);
    SDK::FRotator r{};
    r.Yaw = std::atan2(p.Y - at.Y, p.X - at.X) * 180.0f / 3.14159265f;
    pc->SetControlRotation(r);
    QD_INFO("edbg", "goto {} at ({:.0f},{:.0f},{:.0f}), {:.0f} cm away", ue::ClassName(e->enemy), p.X, p.Y, p.Z, dist);
    return true;
}

void DebugEnemyHp(const std::string& filter, float fraction) {
    Entry* e = DebugFind(filter);
    if (!e) return;
    if (!IsMine(e->id) && S().active) {
        QD_WARN("edbg", "hp: {} belongs to P{}, set it there", ue::ClassName(e->enemy), OwnerOf(e->id).slot + 1);
        return;
    }
    const float mx = QueryMaxHp(e->enemy);
    e->enemy->HP = std::max(1.0f, mx * fraction);
    QD_INFO("edbg", "hp {} -> {:.0f}/{:.0f}", ue::ClassName(e->enemy), e->enemy->HP, mx);
}

void DebugEnemyHit(const std::string& filter, float damage) {
    auto& s = S();
    Entry* e = DebugFind(filter);
    if (!e) return;
    const uint8_t owner = OwnerOf(e->id).slot;
    if (s.active && owner != s.localSlot) {
        SDK::FFB_DamageAttributeSet d{};
        d.DamageAmount = damage;
        SendDamageRequest(*e, owner, d);
        QD_INFO("edbg", "hit {} for {:.0f}: sent to its owner P{}", ue::ClassName(e->enemy), damage, owner + 1);
    } else {
        net::MsgDamageRequest m;
        m.enemyId = e->id;
        m.damage = damage;
        m.attackerSlot = s.localSlot;
        ApplyRemoteDamage(*e, m);
        NoteLocalHit(*e, damage);
        QD_INFO("edbg", "hit {} for {:.0f}: applied here (owner), hp now {:.0f}", ue::ClassName(e->enemy), damage, e->enemy->HP);
    }
}

void DebugEnemyWake(const std::string& filter) {
    Entry* e = DebugFind(filter);
    if (!e) return;
    SDK::Params::EnemyBase_C_ActivateTrap p{};
    Call(e->enemy, "ActivateTrap", p);
    QD_INFO("edbg", "wake {} (trap now {})", ue::ClassName(e->enemy), e->enemy->InTrapMode ? 1 : 0);
}

void DebugEnemyInteract(const std::string& filter) {
    Entry* e = DebugFind(filter);
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!e || !ch) return;
    struct { SDK::AActor* PlayerInteracting; } p{ch};
    const bool ok = Call(e->enemy, "Interact", p);
    QD_INFO("edbg", "interact {} -> {}", ue::ClassName(e->enemy), ok ? "called" : "no Interact function");
}
#endif   // QD_DEV

}  // namespace qd::sync
