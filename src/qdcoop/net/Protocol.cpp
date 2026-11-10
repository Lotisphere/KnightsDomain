// Knights' Domain — wire protocol codec. Field order == struct declaration order in Protocol.h.
#include "qdcoop/net/Protocol.h"

#include "qdcoop/net/ByteStream.h"

#include <algorithm>
#include <utility>

namespace qd::net {
namespace {

// Encoded sizes of fixed-size pieces (used for sanity checks and EncodeSplit).
constexpr size_t kEnemyStatesHeaderBytes = 1 + 2 + 1 + 2;                      // type, seq, ownerSlot, count
constexpr size_t kEnemyStateBytes = 8 + 12 + 4 + 4 + 4 + 1 + 1 + 1 + 1;        // 36
constexpr size_t kMinStringBytes = 2;                                           // u16 length

ByteWriter Begin(MsgType t, size_t reserve = 16) {
    ByteWriter w(reserve);
    w.U8(static_cast<uint8_t>(t));
    return w;
}

bool BeginRead(ByteReader& r, MsgType t) {
    uint8_t v = 0;
    return r.U8(v) && v == static_cast<uint8_t>(t);
}

void WriteVec(ByteWriter& w, const Vec3f& v) {
    w.F32(v.x);
    w.F32(v.y);
    w.F32(v.z);
}
bool ReadVec(ByteReader& r, Vec3f& v) { return r.F32(v.x) && r.F32(v.y) && r.F32(v.z); }

void WriteName(ByteWriter& w, const std::string& s) { w.Str(s, kMaxNameBytes); }
bool ReadName(ByteReader& r, std::string& s) { return r.Str(s, kMaxNameBytes); }

bool ReadSlot(ByteReader& r, uint8_t& slot) {
    return r.U8(slot) && slot < static_cast<uint8_t>(kMaxPlayers);
}

template <class E>
bool ReadEnum(ByteReader& r, E& out, E lo, E hi) {
    uint8_t v = 0;
    if (!r.U8(v)) return false;
    if (v < static_cast<uint8_t>(lo) || v > static_cast<uint8_t>(hi)) return false;
    out = static_cast<E>(v);
    return true;
}

void WritePeerEntry(ByteWriter& w, const PeerEntry& e) {
    w.U8(e.slot);
    w.U64(e.peerId);
    WriteName(w, e.name);
}
bool ReadPeerEntry(ByteReader& r, PeerEntry& e) { return ReadSlot(r, e.slot) && r.U64(e.peerId) && ReadName(r, e.name); }

void WriteEnemyState(ByteWriter& w, const EnemyState& e) {
    w.U64(e.enemyId);
    WriteVec(w, e.pos);
    w.F32(e.yaw);
    w.F32(e.hp);
    w.F32(e.maxHp);
    w.U8(e.stateIndex);
    w.U8(e.subState);
    w.U8(e.flags);
    w.U8(e.targetSlot);
}
bool ReadEnemyState(ByteReader& r, EnemyState& e) {
    if (!(r.U64(e.enemyId) && ReadVec(r, e.pos) && r.F32(e.yaw) && r.F32(e.hp) && r.F32(e.maxHp) &&
          r.U8(e.stateIndex) && r.U8(e.subState) && r.U8(e.flags) && r.U8(e.targetSlot)))
        return false;
    return e.targetSlot < kMaxPlayers || e.targetSlot == 0xFF;
}

// Decode helper: parse into a temporary so `out` is untouched on failure, and require the whole buffer be consumed.
template <class T, class F>
bool DecodeInto(const uint8_t* data, size_t size, MsgType type, T& out, F&& body) {
    ByteReader r(data, size);
    T m{};
    if (!BeginRead(r, type) || !body(r, m) || !r.AtEnd()) return false;
    out = std::move(m);
    return true;
}

}  // namespace

MsgType PeekType(const uint8_t* data, size_t size) {
    if (!data || size == 0) return static_cast<MsgType>(0);
    return static_cast<MsgType>(data[0]);
}

// ---------------------------------------------------------------- handshake

bool PeekHelloProtocol(const uint8_t* data, size_t size, uint16_t& protocol) {
    if (!data) return false;
    ByteReader r(data, size);
    uint16_t   p = 0;
    if (!BeginRead(r, MsgType::Hello) || !r.U16(p)) return false;
    protocol = p;
    return true;
}

std::vector<uint8_t> Encode(const MsgHello& m) {
    ByteWriter w = Begin(MsgType::Hello, 64);
    w.U16(m.protocol);
    w.Str(m.modName);
    w.Str(m.modAuthor);
    w.Str(m.modVersion);
    w.Str(m.gameBuild);
    WriteName(w, m.name);
    w.U8(static_cast<uint8_t>(m.saveMode));
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgHello& out) {
    return DecodeInto(data, size, MsgType::Hello, out, [](ByteReader& r, MsgHello& m) {
        return r.U16(m.protocol) && r.Str(m.modName) && r.Str(m.modAuthor) && r.Str(m.modVersion) &&
               r.Str(m.gameBuild) && ReadName(r, m.name) && ReadEnum(r, m.saveMode, SaveMode::OwnSave, SaveMode::CoopCopy);
    });
}

std::vector<uint8_t> Encode(const MsgReject& m) {
    ByteWriter w = Begin(MsgType::Reject);
    w.U8(static_cast<uint8_t>(m.reason));
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgReject& out) {
    return DecodeInto(data, size, MsgType::Reject, out, [](ByteReader& r, MsgReject& m) {
        return ReadEnum(r, m.reason, RejectReason::Full, RejectReason::Kicked);
    });
}

std::vector<uint8_t> Encode(const MsgWelcome& m) {
    ByteWriter w = Begin(MsgType::Welcome, 128);
    w.U8(m.yourSlot);
    const uint16_t n = w.Count(m.peers.size());
    for (uint16_t i = 0; i < n; ++i) WritePeerEntry(w, m.peers[i]);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgWelcome& out) {
    return DecodeInto(data, size, MsgType::Welcome, out, [](ByteReader& r, MsgWelcome& m) {
        uint16_t n = 0;
        if (!ReadSlot(r, m.yourSlot) || !r.Count(n, 1 + 8 + kMinStringBytes)) return false;
        if (n > kMaxPlayers) return false;
        m.peers.resize(n);
        for (auto& e : m.peers)
            if (!ReadPeerEntry(r, e)) return false;
        return true;
    });
}

std::vector<uint8_t> Encode(const MsgPeerJoined& m) {
    ByteWriter w = Begin(MsgType::PeerJoined, 32);
    WritePeerEntry(w, m.peer);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgPeerJoined& out) {
    return DecodeInto(data, size, MsgType::PeerJoined, out,
                      [](ByteReader& r, MsgPeerJoined& m) { return ReadPeerEntry(r, m.peer); });
}

std::vector<uint8_t> Encode(const MsgPeerLeft& m) {
    ByteWriter w = Begin(MsgType::PeerLeft);
    w.U8(m.slot);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgPeerLeft& out) {
    return DecodeInto(data, size, MsgType::PeerLeft, out,
                      [](ByteReader& r, MsgPeerLeft& m) { return ReadSlot(r, m.slot); });
}

std::vector<uint8_t> Encode(const MsgNameChanged& m) {
    ByteWriter w = Begin(MsgType::NameChanged, 32);
    w.U8(m.slot);
    WriteName(w, m.name);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgNameChanged& out) {
    return DecodeInto(data, size, MsgType::NameChanged, out,
                      [](ByteReader& r, MsgNameChanged& m) { return ReadSlot(r, m.slot) && ReadName(r, m.name); });
}

// ---------------------------------------------------------------- players

// 1 + 1 + 2 + 12 + 4 + 4 + 12 + 1 + 2 + 2 + 1 + 2 = 44 bytes (requirement: <= 64).
std::vector<uint8_t> Encode(const MsgPlayerState& m) {
    ByteWriter w = Begin(MsgType::PlayerState, 48);
    w.U8(m.slot);
    w.U16(m.seq);
    WriteVec(w, m.pos);
    w.F32(m.yaw);
    w.F32(m.pitch);
    WriteVec(w, m.vel);
    w.U8(m.moveMode);
    w.U16(m.actionId);
    w.U16(m.weaponId);
    w.U8(m.hpPct);
    w.U16(m.areaId);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgPlayerState& out) {
    return DecodeInto(data, size, MsgType::PlayerState, out, [](ByteReader& r, MsgPlayerState& m) {
        return ReadSlot(r, m.slot) && r.U16(m.seq) && ReadVec(r, m.pos) && r.F32(m.yaw) && r.F32(m.pitch) &&
               ReadVec(r, m.vel) && r.U8(m.moveMode) && r.U16(m.actionId) && r.U16(m.weaponId) && r.U8(m.hpPct) &&
               m.hpPct <= 100 && r.U16(m.areaId);
    });
}

std::vector<uint8_t> Encode(const MsgPlayerEvent& m) {
    ByteWriter w = Begin(MsgType::PlayerEvent, 24);
    w.U8(m.slot);
    w.U8(static_cast<uint8_t>(m.type));
    w.I32(m.arg);
    WriteVec(w, m.pos);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgPlayerEvent& out) {
    return DecodeInto(data, size, MsgType::PlayerEvent, out, [](ByteReader& r, MsgPlayerEvent& m) {
        return ReadSlot(r, m.slot) && ReadEnum(r, m.type, PlayerEventType::Attack, PlayerEventType::ExitUI) &&
               r.I32(m.arg) && ReadVec(r, m.pos);
    });
}

// [u8 type][u8 slot][u16 len][path bytes], len <= kMaxItemPathBytes.
std::vector<uint8_t> Encode(const MsgEquip& m) {
    ByteWriter w = Begin(MsgType::Equip, 4 + std::min(m.itemPath.size(), kMaxItemPathBytes));
    w.U8(m.slot);
    w.Str(m.itemPath, kMaxItemPathBytes);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgEquip& out) {
    return DecodeInto(data, size, MsgType::Equip, out, [](ByteReader& r, MsgEquip& m) {
        return ReadSlot(r, m.slot) && r.Str(m.itemPath, kMaxItemPathBytes);
    });
}

// ---------------------------------------------------------------- enemies

std::vector<uint8_t> Encode(const MsgEnemyStates& m) {
    ByteWriter w = Begin(MsgType::EnemyStates, kEnemyStatesHeaderBytes + m.enemies.size() * kEnemyStateBytes);
    w.U16(m.seq);
    w.U8(m.ownerSlot);
    const uint16_t n = w.Count(m.enemies.size());
    for (uint16_t i = 0; i < n; ++i) WriteEnemyState(w, m.enemies[i]);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgEnemyStates& out) {
    return DecodeInto(data, size, MsgType::EnemyStates, out, [](ByteReader& r, MsgEnemyStates& m) {
        uint16_t n = 0;
        if (!r.U16(m.seq) || !ReadSlot(r, m.ownerSlot) || !r.Count(n, kEnemyStateBytes)) return false;
        m.enemies.resize(n);
        for (auto& e : m.enemies)
            if (!ReadEnemyState(r, e)) return false;
        return true;
    });
}

std::vector<std::vector<uint8_t>> EncodeSplit(const MsgEnemyStates& m, size_t maxBytes) {
    size_t perChunk = maxBytes > kEnemyStatesHeaderBytes ? (maxBytes - kEnemyStatesHeaderBytes) / kEnemyStateBytes : 0;
    perChunk = std::clamp<size_t>(perChunk, 1, 0xFFFF);

    std::vector<std::vector<uint8_t>> out;
    if (m.enemies.empty()) {
        out.push_back(Encode(m));
        return out;
    }
    out.reserve((m.enemies.size() + perChunk - 1) / perChunk);
    for (size_t first = 0; first < m.enemies.size(); first += perChunk) {
        const size_t n = std::min(perChunk, m.enemies.size() - first);
        ByteWriter w = Begin(MsgType::EnemyStates, kEnemyStatesHeaderBytes + n * kEnemyStateBytes);
        w.U16(m.seq);
        w.U8(m.ownerSlot);
        w.Count(n);
        for (size_t i = 0; i < n; ++i) WriteEnemyState(w, m.enemies[first + i]);
        out.push_back(w.Take());
    }
    return out;
}

std::vector<uint8_t> Encode(const MsgEnemyEvent& m) {
    ByteWriter w = Begin(MsgType::EnemyEvent);
    w.U64(m.enemyId);
    w.U8(static_cast<uint8_t>(m.type));
    w.I32(m.arg);
    w.U32(m.seed);
    w.U8(m.ownerSlot);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgEnemyEvent& out) {
    return DecodeInto(data, size, MsgType::EnemyEvent, out, [](ByteReader& r, MsgEnemyEvent& m) {
        return r.U64(m.enemyId) && ReadEnum(r, m.type, EnemyEventType::AttackStart, EnemyEventType::Interacted) &&
               r.I32(m.arg) && r.U32(m.seed) && ReadSlot(r, m.ownerSlot);
    });
}

std::vector<uint8_t> Encode(const MsgDamageRequest& m) {
    ByteWriter w = Begin(MsgType::DamageRequest, 128);
    w.U64(m.enemyId);
    w.F32(m.damage);
    const uint16_t nd = w.Count(m.damageTags.size());
    for (uint16_t i = 0; i < nd; ++i) w.Str(m.damageTags[i]);
    const uint16_t nv = w.Count(m.dataTags.size());
    for (uint16_t i = 0; i < nv; ++i) {
        w.Str(m.dataTags[i].tag);
        w.F32(m.dataTags[i].value);
    }
    for (uint32_t g : m.sourceGuid) w.U32(g);
    w.U8(m.attackerSlot);
    w.U8(m.flags);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgDamageRequest& out) {
    return DecodeInto(data, size, MsgType::DamageRequest, out, [](ByteReader& r, MsgDamageRequest& m) {
        uint16_t n = 0;
        if (!r.U64(m.enemyId) || !r.F32(m.damage) || !r.Count(n, kMinStringBytes)) return false;
        m.damageTags.resize(n);
        for (auto& t : m.damageTags)
            if (!r.Str(t)) return false;
        if (!r.Count(n, kMinStringBytes + 4)) return false;
        m.dataTags.resize(n);
        for (auto& t : m.dataTags)
            if (!r.Str(t.tag) || !r.F32(t.value)) return false;
        for (auto& g : m.sourceGuid)
            if (!r.U32(g)) return false;
        return ReadSlot(r, m.attackerSlot) && r.U8(m.flags) && (m.flags & ~(kDamageHitNotifyOnly | kDamageForwarded)) == 0;
    });
}

// [u8 type][u64 enemyId][u8 ownerSlot][f32 hp][f32 maxHp][vec3 pos][f32 yaw] = 34 bytes.
std::vector<uint8_t> Encode(const MsgEnemyOwner& m) {
    ByteWriter w = Begin(MsgType::EnemyOwner, 40);
    w.U64(m.enemyId);
    w.U8(m.ownerSlot);
    w.F32(m.hp);
    w.F32(m.maxHp);
    WriteVec(w, m.pos);
    w.F32(m.yaw);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgEnemyOwner& out) {
    return DecodeInto(data, size, MsgType::EnemyOwner, out, [](ByteReader& r, MsgEnemyOwner& m) {
        return r.U64(m.enemyId) && ReadSlot(r, m.ownerSlot) && r.F32(m.hp) && r.F32(m.maxHp) && ReadVec(r, m.pos) &&
               r.F32(m.yaw);
    });
}

// [u8 type][u64 enemyId][u8 ownerSlot][f32 rate][str path]
std::vector<uint8_t> Encode(const MsgEnemyMontage& m) {
    ByteWriter w = Begin(MsgType::EnemyMontage, 24 + m.path.size());
    w.U64(m.enemyId);
    w.U8(m.ownerSlot);
    w.F32(m.rate);
    w.Str(m.path, kMaxMontagePathBytes);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgEnemyMontage& out) {
    return DecodeInto(data, size, MsgType::EnemyMontage, out, [](ByteReader& r, MsgEnemyMontage& m) {
        return r.U64(m.enemyId) && ReadSlot(r, m.ownerSlot) && r.F32(m.rate) && r.Str(m.path, kMaxMontagePathBytes);
    });
}

// [u8 type][u64 enemyId][u8 ownerSlot][u32 layout][u16 n][n bytes]
std::vector<uint8_t> Encode(const MsgEnemyBlob& m) {
    const size_t n = std::min(m.data.size(), kMaxEnemyBlobBytes);
    ByteWriter w = Begin(MsgType::EnemyBlob, 16 + n);
    w.U64(m.enemyId);
    w.U8(m.ownerSlot);
    w.U32(m.layout);
    w.U16(static_cast<uint16_t>(n));
    w.Bytes(m.data.data(), n);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgEnemyBlob& out) {
    return DecodeInto(data, size, MsgType::EnemyBlob, out, [](ByteReader& r, MsgEnemyBlob& m) {
        uint16_t n = 0;
        if (!(r.U64(m.enemyId) && ReadSlot(r, m.ownerSlot) && r.U32(m.layout) && r.U16(n))) return false;
        if (n > kMaxEnemyBlobBytes) return false;
        m.data.resize(n);
        for (uint16_t i = 0; i < n; ++i)
            if (!r.U8(m.data[i])) return false;
        return true;
    });
}

// ---------------------------------------------------------------- world

std::vector<uint8_t> Encode(const MsgWorldFlags& m) {
    ByteWriter w = Begin(MsgType::WorldFlags, 4 + m.flags.size() * 24);
    w.Bool(m.full);
    const uint16_t n = w.Count(m.flags.size());
    for (uint16_t i = 0; i < n; ++i) {
        w.Str(m.flags[i].name);
        w.I32(m.flags[i].value);
    }
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgWorldFlags& out) {
    return DecodeInto(data, size, MsgType::WorldFlags, out, [](ByteReader& r, MsgWorldFlags& m) {
        uint16_t n = 0;
        if (!r.Bool(m.full) || !r.Count(n, kMinStringBytes + 4)) return false;
        m.flags.resize(n);
        for (auto& f : m.flags)
            if (!r.Str(f.name) || !r.I32(f.value)) return false;
        return true;
    });
}

std::vector<uint8_t> Encode(const MsgInteractRequest& m) {
    ByteWriter w = Begin(MsgType::InteractRequest, 8 + m.actorPath.size());
    w.U32(m.requestId);
    w.Str(m.actorPath);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgInteractRequest& out) {
    return DecodeInto(data, size, MsgType::InteractRequest, out,
                      [](ByteReader& r, MsgInteractRequest& m) { return r.U32(m.requestId) && r.Str(m.actorPath); });
}

std::vector<uint8_t> Encode(const MsgInteractResult& m) {
    ByteWriter w = Begin(MsgType::InteractResult);
    w.U32(m.requestId);
    w.U8(static_cast<uint8_t>(m.outcome));
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgInteractResult& out) {
    return DecodeInto(data, size, MsgType::InteractResult, out, [](ByteReader& r, MsgInteractResult& m) {
        return r.U32(m.requestId) &&
               ReadEnum(r, m.outcome, InteractOutcome::Allowed, InteractOutcome::DoneByHost);
    });
}

std::vector<uint8_t> Encode(const MsgHpScale& m) {
    ByteWriter w = Begin(MsgType::HpScale);
    w.F32(m.normal);
    w.F32(m.elite);
    w.F32(m.boss);
    w.U8(m.playerCount);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgHpScale& out) {
    return DecodeInto(data, size, MsgType::HpScale, out, [](ByteReader& r, MsgHpScale& m) {
        return r.F32(m.normal) && r.F32(m.elite) && r.F32(m.boss) && r.U8(m.playerCount) && m.playerCount >= 1 &&
               m.playerCount <= kMaxPlayers;
    });
}

std::vector<uint8_t> Encode(const MsgSummon& m) {
    ByteWriter w = Begin(MsgType::Summon, 24 + m.areaName.size());
    WriteVec(w, m.pos);
    w.F32(m.yaw);
    w.Str(m.areaName);
    return w.Take();
}
bool Decode(const uint8_t* data, size_t size, MsgSummon& out) {
    return DecodeInto(data, size, MsgType::Summon, out, [](ByteReader& r, MsgSummon& m) {
        return ReadVec(r, m.pos) && r.F32(m.yaw) && r.Str(m.areaName);
    });
}

}  // namespace qd::net
