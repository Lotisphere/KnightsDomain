// Knights' Domain — wire protocol contract (v2 = enemy ownership; v3 = mod name/author in Hello). Little-endian. Every message starts with [u8 MsgType].
// Encode/Decode must round-trip exactly; Decode returns false on any truncation or out-of-range value.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace qd::net {

constexpr uint16_t kProtocolVersion = 5;   // v5: EnemyBlob, EnemyEvent Interacted (v4: Woke/CustomAttack, EnemyState flag bit4)
constexpr int      kMaxPlayers = 4;
constexpr size_t   kMaxNameBytes = 72;    // UTF-8, 24 CJK chars
constexpr size_t   kMaxItemPathBytes = 256;   // MsgEquip::itemPath

enum class MsgType : uint8_t {
    Hello = 1, Welcome, Reject, PeerJoined, PeerLeft, NameChanged,
    PlayerState = 20, PlayerEvent, Equip = 22,
    EnemyStates = 40, EnemyEvent, DamageRequest, EnemyOwner = 45, EnemyBlob = 46, EnemyMontage = 47,
    WorldFlags = 60, InteractRequest, InteractResult, HpScale, Summon,
    Heartbeat = 250,   // consumed by Session
};

struct Vec3f { float x = 0, y = 0, z = 0; };

// ---- handshake ----
enum class SaveMode : uint8_t { OwnSave = 0, CoopCopy = 1 };
// modName / modAuthor are checked by the host against core::kModName / kModAuthor ("Knights' Domain" /
// "Mangolotis"); a mismatch is rejected as RejectReason::ModVersion. Wire order = declaration order (saveMode last).
struct MsgHello   {
    uint16_t    protocol = kProtocolVersion;
    std::string modName, modAuthor, modVersion, gameBuild, name;
    SaveMode    saveMode = SaveMode::OwnSave;
};
enum class RejectReason : uint8_t { Full = 1, ModVersion, GameBuild, Protocol, Kicked };
struct MsgReject  { RejectReason reason = RejectReason::Full; };
struct PeerEntry  { uint8_t slot = 0; uint64_t peerId = 0; std::string name; };
struct MsgWelcome { uint8_t yourSlot = 0; std::vector<PeerEntry> peers; };          // peers includes the host (slot 0)
struct MsgPeerJoined  { PeerEntry peer; };
struct MsgPeerLeft    { uint8_t slot = 0; };
struct MsgNameChanged { uint8_t slot = 0; std::string name; };

// ---- players ----
struct MsgPlayerState {          // Unreliable, 20 Hz. Host relays clients' states to the other clients.
    uint8_t  slot = 0;
    uint16_t seq = 0;            // wraps; receivers drop older-than-latest (serial-number arithmetic)
    Vec3f    pos;                // cm, world
    float    yaw = 0, pitch = 0; // degrees
    Vec3f    vel;
    uint8_t  moveMode = 0;       // EMovementMode
    uint16_t actionId = 0;       // animation action id
    uint16_t weaponId = 0;       // index into the shared weapon table, 0 = none
    uint8_t  hpPct = 100;        // 0..100
    uint16_t areaId = 0;         // hash16 of the current area name
};
enum class PlayerEventType : uint8_t { Attack = 1, Hit, Died, Respawned, Teleported, EnterUI, ExitUI };
struct MsgPlayerEvent { uint8_t slot = 0; PlayerEventType type = PlayerEventType::Attack; int32_t arg = 0; Vec3f pos; };
// Reliable, on change + once to every member that joins. Host relays clients' Equip to the other clients (slot
// rewritten to the sender's real slot). itemPath = path of the in-hand weapon's FS_ItemData_C asset, <= kMaxItemPathBytes
// (Encode cuts longer strings at a UTF-8 boundary, Decode rejects them); "" = empty-handed.
struct MsgEquip { uint8_t slot = 0; std::string itemPath; };

// ---- enemies ----
struct EnemyState {
    uint64_t enemyId = 0;        // SpawnerGUID-derived or FNV-1a64(actor path)
    Vec3f    pos;
    float    yaw = 0;
    float    hp = 0, maxHp = 0;
    uint8_t  stateIndex = 0, subState = 0;
    uint8_t  flags = 0;          // bit0 dead, bit1 staggered, bit2 aggro (aggro'd / chasing / attacking), bit3 boss,
                                 // bit4 dormant (EnemyBase.InTrapMode: lying / not risen yet)
                                 // bit5 woken (ActivateTrap has run on the owner's copy), bit6 boss bar shown
    uint8_t  targetSlot = 0xFF;  // 0xFF = none
};
// sent by the enemies' simulator (owner). ownerSlot = the sender; the host overwrites it with the real sender's slot
// when relaying a client's snapshot to the other clients. Every chunk of an EncodeSplit carries the same seq + ownerSlot.
struct MsgEnemyStates { uint16_t seq = 0; uint8_t ownerSlot = 0; std::vector<EnemyState> enemies; };   // keep encoded size <= 1100 bytes; split otherwise
// Woke: the sender's copy left trap mode (risen / boss fight started). Accepted from ANY slot: waking is sticky, every
// receiver wakes its own copy. CustomAttack: the owner ran a BP attack function that plays its own montage and drives
// its own hitboxes (e.g. the Grave Guardian's DoNormalAttack / DoBigAttack); arg = index into the custom-attack table.
// Interacted: the sender's player started talking to / interacting with this enemy (dialogue-started boss fights).
// Accepted from any slot; receivers only show a notice.
enum class EnemyEventType : uint8_t { AttackStart = 1, Staggered, Died, BossBarOn, BossBarOff, Woke, CustomAttack, Interacted };
// seed: AttackStart only — the owner seeded the game's rand() with it right before running the attack, a follower that
// replays the attack's Blueprint (elite / boss) seeds the same, so random attack choices come out the same.
struct MsgEnemyEvent  { uint64_t enemyId = 0; EnemyEventType type = EnemyEventType::AttackStart; int32_t arg = 0; uint32_t seed = 0; uint8_t ownerSlot = 0; };
struct TagValue       { std::string tag; float value = 0; };
// bit1: the first owner no longer owned the enemy when the hit arrived (ownership changed on the way) and sent it back
// to the host once, which settles it with the current owner. attackerSlot is kept as the host first wrote it.
constexpr uint8_t kDamageForwarded = 2;
constexpr uint8_t kDamageHitNotifyOnly = 1;   // MsgDamageRequest::flags bit0: "slot X hit enemy Y" (host records aggro only)
struct MsgDamageRequest {
    uint64_t enemyId = 0;
    float    damage = 0;
    std::vector<std::string> damageTags;   // FGameplayTag names
    std::vector<TagValue>    dataTags;
    std::array<uint32_t, 4>  sourceGuid{};
    uint8_t  attackerSlot = 0;             // host rewrites it with the real sender when relaying to the enemy's owner
    uint8_t  flags = 0;                    // kDamageHitNotifyOnly; other bits must be 0
};
// host -> clients, reliable, whenever an enemy's simulator changes (and to a newcomer for every ownerSlot != 0).
// hp/maxHp/pos/yaw = the host's copy at the moment of the change (the new owner aligns to it when it has no fresher snapshot).
struct MsgEnemyOwner { uint64_t enemyId = 0; uint8_t ownerSlot = 0; float hp = 0, maxHp = 0; Vec3f pos; float yaw = 0; };

// the owner's copy of a boss / elite keeps private Blueprint state (phase counters, "horn broken", ...). The owner
// sends those plain bool / int / float / byte variables about once a second while the enemy is between attacks; the
// others keep the latest blob and apply it to their copy, and a new owner starts from it. layout = hash of the
// variable names and sizes (both ends run the same game build; a mismatch is ignored). Unreliable; host relays.
// the montage the owner's copy is playing (path of the UAnimMontage, "" = it stopped). Sent when it changes;
// followers play the same asset, so every boss animation — attacks that bypass BP_StartAttackState, intros, phase
// changes, roars — shows on every machine, with the montage's own notifies (hitboxes). Reliable; host relays.
constexpr size_t kMaxMontagePathBytes = 220;
struct MsgEnemyMontage { uint64_t enemyId = 0; uint8_t ownerSlot = 0; float rate = 1; std::string path; };

constexpr size_t kMaxEnemyBlobBytes = 1000;
struct MsgEnemyBlob { uint64_t enemyId = 0; uint8_t ownerSlot = 0; uint32_t layout = 0; std::vector<uint8_t> data; };

// ---- world ----
struct FlagValue          { std::string name; int32_t value = 0; };
struct MsgWorldFlags      { bool full = false; std::vector<FlagValue> flags; };
struct MsgInteractRequest { uint32_t requestId = 0; std::string actorPath; };
enum class InteractOutcome : uint8_t { Allowed = 1, Denied, DoneByHost };
struct MsgInteractResult  { uint32_t requestId = 0; InteractOutcome outcome = InteractOutcome::Denied; };
struct MsgHpScale         { float normal = 1, elite = 1, boss = 1; uint8_t playerCount = 1; };
struct MsgSummon          { Vec3f pos; float yaw = 0; std::string areaName; };

// ---- codec ----
MsgType PeekType(const uint8_t* data, size_t size);   // returns (MsgType)0 if size == 0
// Reads only [u8 Hello][u16 protocol] of a Hello, so a host can answer a Hello of another protocol version (which it
// cannot Decode) with Reject(Protocol). False if `data` is not a Hello or is too short.
bool PeekHelloProtocol(const uint8_t* data, size_t size, uint16_t& protocol);

#define QD_MSG(T) \
    std::vector<uint8_t> Encode(const T& m); \
    bool Decode(const uint8_t* data, size_t size, T& out);
QD_MSG(MsgHello) QD_MSG(MsgReject) QD_MSG(MsgWelcome) QD_MSG(MsgPeerJoined) QD_MSG(MsgPeerLeft) QD_MSG(MsgNameChanged)
QD_MSG(MsgPlayerState) QD_MSG(MsgPlayerEvent) QD_MSG(MsgEquip)
QD_MSG(MsgEnemyStates) QD_MSG(MsgEnemyEvent) QD_MSG(MsgDamageRequest) QD_MSG(MsgEnemyOwner) QD_MSG(MsgEnemyBlob) QD_MSG(MsgEnemyMontage)
QD_MSG(MsgWorldFlags) QD_MSG(MsgInteractRequest) QD_MSG(MsgInteractResult) QD_MSG(MsgHpScale) QD_MSG(MsgSummon)
#undef QD_MSG

// Splits a large enemy snapshot into several complete MsgEnemyStates messages, each <= maxBytes when encoded
// (at least one enemy per message even if maxBytes is absurdly small). Every chunk carries the same m.seq, so
// receivers must treat an equal seq as "same snapshot" (only drop strictly older ones). Empty input -> one message.
std::vector<std::vector<uint8_t>> EncodeSplit(const MsgEnemyStates& m, size_t maxBytes = 1100);

}  // namespace qd::net
