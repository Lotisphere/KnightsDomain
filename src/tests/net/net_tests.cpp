// QD Coop — net layer unit tests. No framework: CHECK prints each failure; exit code 0 = all passed, 1 = any failed.
// Uses UDP ports 27998 and 27999 on this machine (Windows may show a firewall prompt the first time; loopback works
// either way).
#include "qdcoop/net/ByteStream.h"
#include "qdcoop/net/Protocol.h"
#include "qdcoop/net/Session.h"
#include "qdcoop/net/SessionInternal.h"
#include "qdcoop/net/Transport.h"
#include "qdcoop/net/UdpTransport.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace qd::net;
using Clock = std::chrono::steady_clock;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

void Section(const char* name) { std::printf("[%s]\n", name); }

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ------------------------------------------------------------------ equality (Protocol.h structs have no operator==)

bool Eq(const Vec3f& a, const Vec3f& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Eq(const PeerEntry& a, const PeerEntry& b) { return a.slot == b.slot && a.peerId == b.peerId && a.name == b.name; }
bool Eq(const MsgHello& a, const MsgHello& b) {
    return a.protocol == b.protocol && a.modName == b.modName && a.modAuthor == b.modAuthor &&
           a.modVersion == b.modVersion && a.gameBuild == b.gameBuild && a.name == b.name && a.saveMode == b.saveMode;
}
bool Eq(const MsgReject& a, const MsgReject& b) { return a.reason == b.reason; }
bool Eq(const MsgWelcome& a, const MsgWelcome& b) {
    if (a.yourSlot != b.yourSlot || a.peers.size() != b.peers.size()) return false;
    for (size_t i = 0; i < a.peers.size(); ++i)
        if (!Eq(a.peers[i], b.peers[i])) return false;
    return true;
}
bool Eq(const MsgPeerJoined& a, const MsgPeerJoined& b) { return Eq(a.peer, b.peer); }
bool Eq(const MsgPeerLeft& a, const MsgPeerLeft& b) { return a.slot == b.slot; }
bool Eq(const MsgNameChanged& a, const MsgNameChanged& b) { return a.slot == b.slot && a.name == b.name; }
bool Eq(const MsgPlayerState& a, const MsgPlayerState& b) {
    return a.slot == b.slot && a.seq == b.seq && Eq(a.pos, b.pos) && a.yaw == b.yaw && a.pitch == b.pitch &&
           Eq(a.vel, b.vel) && a.moveMode == b.moveMode && a.actionId == b.actionId && a.weaponId == b.weaponId &&
           a.hpPct == b.hpPct && a.areaId == b.areaId;
}
bool Eq(const MsgPlayerEvent& a, const MsgPlayerEvent& b) {
    return a.slot == b.slot && a.type == b.type && a.arg == b.arg && Eq(a.pos, b.pos);
}
bool Eq(const MsgEquip& a, const MsgEquip& b) { return a.slot == b.slot && a.itemPath == b.itemPath; }
bool Eq(const EnemyState& a, const EnemyState& b) {
    return a.enemyId == b.enemyId && Eq(a.pos, b.pos) && a.yaw == b.yaw && a.hp == b.hp && a.maxHp == b.maxHp &&
           a.stateIndex == b.stateIndex && a.subState == b.subState && a.flags == b.flags &&
           a.targetSlot == b.targetSlot;
}
bool Eq(const MsgEnemyStates& a, const MsgEnemyStates& b) {
    if (a.seq != b.seq || a.ownerSlot != b.ownerSlot || a.enemies.size() != b.enemies.size()) return false;
    for (size_t i = 0; i < a.enemies.size(); ++i)
        if (!Eq(a.enemies[i], b.enemies[i])) return false;
    return true;
}
bool Eq(const MsgEnemyEvent& a, const MsgEnemyEvent& b) {
    return a.enemyId == b.enemyId && a.type == b.type && a.arg == b.arg && a.seed == b.seed && a.ownerSlot == b.ownerSlot;
}
bool Eq(const MsgEnemyMontage& a, const MsgEnemyMontage& b) {
    return a.enemyId == b.enemyId && a.ownerSlot == b.ownerSlot && a.rate == b.rate && a.path == b.path;
}
bool Eq(const MsgEnemyBlob& a, const MsgEnemyBlob& b) {
    return a.enemyId == b.enemyId && a.ownerSlot == b.ownerSlot && a.layout == b.layout && a.data == b.data;
}
bool Eq(const MsgDamageRequest& a, const MsgDamageRequest& b) {
    if (a.enemyId != b.enemyId || a.damage != b.damage || a.damageTags != b.damageTags ||
        a.dataTags.size() != b.dataTags.size() || a.sourceGuid != b.sourceGuid || a.attackerSlot != b.attackerSlot ||
        a.flags != b.flags)
        return false;
    for (size_t i = 0; i < a.dataTags.size(); ++i)
        if (a.dataTags[i].tag != b.dataTags[i].tag || a.dataTags[i].value != b.dataTags[i].value) return false;
    return true;
}
bool Eq(const MsgEnemyOwner& a, const MsgEnemyOwner& b) {
    return a.enemyId == b.enemyId && a.ownerSlot == b.ownerSlot && a.hp == b.hp && a.maxHp == b.maxHp &&
           Eq(a.pos, b.pos) && a.yaw == b.yaw;
}
bool Eq(const MsgWorldFlags& a, const MsgWorldFlags& b) {
    if (a.full != b.full || a.flags.size() != b.flags.size()) return false;
    for (size_t i = 0; i < a.flags.size(); ++i)
        if (a.flags[i].name != b.flags[i].name || a.flags[i].value != b.flags[i].value) return false;
    return true;
}
bool Eq(const MsgInteractRequest& a, const MsgInteractRequest& b) {
    return a.requestId == b.requestId && a.actorPath == b.actorPath;
}
bool Eq(const MsgInteractResult& a, const MsgInteractResult& b) {
    return a.requestId == b.requestId && a.outcome == b.outcome;
}
bool Eq(const MsgHpScale& a, const MsgHpScale& b) {
    return a.normal == b.normal && a.elite == b.elite && a.boss == b.boss && a.playerCount == b.playerCount;
}
bool Eq(const MsgSummon& a, const MsgSummon& b) {
    return Eq(a.pos, b.pos) && a.yaw == b.yaw && a.areaName == b.areaName;
}

// Encode -> Decode must round-trip; every strict prefix (incl. "truncated by 1 byte") and a trailing extra byte must fail.
template <class T>
void RoundTrip(const char* name, const T& m, MsgType expectedType) {
    const std::vector<uint8_t> bytes = Encode(m);
    T                          back{};
    const bool                 decoded = Decode(bytes.data(), bytes.size(), back);
    if (!decoded) std::printf("  (%s did not decode)\n", name);
    CHECK(decoded);
    CHECK(Eq(m, back));
    CHECK(PeekType(bytes.data(), bytes.size()) == expectedType);

    T    junk{};
    bool anyPrefixDecoded = false;
    for (size_t len = 0; len < bytes.size(); ++len)
        if (Decode(bytes.data(), len, junk)) anyPrefixDecoded = true;
    if (anyPrefixDecoded) std::printf("  (%s: a truncated copy decoded)\n", name);
    CHECK(!anyPrefixDecoded);
    CHECK(!Decode(bytes.data(), bytes.size() - 1, junk));

    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size(), junk));
}

bool IsValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<uint8_t>(s[i]);
        size_t     len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

std::string Repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

const std::string kCjk = "\xE6\x98\xA5";  // 春 (U+6625), 3 bytes in UTF-8

// ------------------------------------------------------------------ 1. round trips

void TestRoundTrips() {
    Section("1. protocol round trips");

    MsgHello hello;
    hello.protocol = kProtocolVersion;
    hello.modName = "Knights' Domain";
    hello.modAuthor = "Mangolotis";
    hello.modVersion = "1.0.0";
    hello.gameBuild = "20261030-123456789";
    hello.name = "Knight" + kCjk;
    hello.saveMode = SaveMode::CoopCopy;
    RoundTrip("Hello", hello, MsgType::Hello);

    MsgReject reject;
    reject.reason = RejectReason::GameBuild;
    RoundTrip("Reject", reject, MsgType::Reject);

    MsgWelcome welcome;
    welcome.yourSlot = 2;
    welcome.peers = {{0, 76561198000000001ull, "Host"}, {1, 76561198000000002ull, kCjk}, {2, 0x7F00007F000001FFull, ""}};
    RoundTrip("Welcome", welcome, MsgType::Welcome);

    MsgPeerJoined joined;
    joined.peer = {3, 76561198000000004ull, "Guest"};
    RoundTrip("PeerJoined", joined, MsgType::PeerJoined);

    MsgPeerLeft left;
    left.slot = 3;
    RoundTrip("PeerLeft", left, MsgType::PeerLeft);

    MsgNameChanged renamed;
    renamed.slot = 1;
    renamed.name = Repeat(kCjk, 5);
    RoundTrip("NameChanged", renamed, MsgType::NameChanged);

    MsgPlayerState ps;
    ps.slot = 1;
    ps.seq = 65535;
    ps.pos = {1234.5f, -6789.25f, 100.125f};
    ps.yaw = 179.5f;
    ps.pitch = -45.25f;
    ps.vel = {300.f, -0.5f, 980.f};
    ps.moveMode = 3;
    ps.actionId = 0xBEEF;
    ps.weaponId = 42;
    ps.hpPct = 77;
    ps.areaId = 0x1234;
    RoundTrip("PlayerState", ps, MsgType::PlayerState);
    const size_t psBytes = Encode(ps).size();
    std::printf("  PlayerState encodes to %zu bytes\n", psBytes);
    CHECK(psBytes <= 64);

    MsgPlayerEvent pe;
    pe.slot = 2;
    pe.type = PlayerEventType::Teleported;
    pe.arg = -12345;
    pe.pos = {1.f, 2.f, 3.f};
    RoundTrip("PlayerEvent", pe, MsgType::PlayerEvent);

    CHECK(static_cast<uint8_t>(MsgType::Equip) == 22);
    MsgEquip eq;
    eq.slot = 3;
    eq.itemPath = "/Game/Items/Weapons/RustySword/FS_ItemData_RustySword.FS_ItemData_RustySword";
    RoundTrip("Equip", eq, MsgType::Equip);
    RoundTrip("Equip(empty hands)", MsgEquip{1, ""}, MsgType::Equip);

    MsgEnemyStates es;
    es.seq = 7;
    es.ownerSlot = 2;
    for (int i = 0; i < 5; ++i) {
        EnemyState e;
        e.enemyId = 0xF00D0000'00000000ull + i;
        e.pos = {i * 100.f, i * -50.f, 12.5f};
        e.yaw = i * 10.f;
        e.hp = 500.f - i;
        e.maxHp = 500.f;
        e.stateIndex = static_cast<uint8_t>(i);
        e.subState = static_cast<uint8_t>(i * 2);
        e.flags = static_cast<uint8_t>(i & 0x0F);
        e.targetSlot = i == 4 ? 0xFF : static_cast<uint8_t>(i % 4);
        es.enemies.push_back(e);
    }
    RoundTrip("EnemyStates", es, MsgType::EnemyStates);
    RoundTrip("EnemyStates(empty)", MsgEnemyStates{}, MsgType::EnemyStates);

    MsgEnemyEvent ee;
    ee.enemyId = 0xCBF29CE484222325ull;
    ee.type = EnemyEventType::BossBarOn;
    ee.arg = 3;
    ee.seed = 0xDEADBEEFu;
    ee.ownerSlot = 3;
    RoundTrip("EnemyEvent", ee, MsgType::EnemyEvent);
    {
        MsgEnemyEvent w = ee;
        w.type = EnemyEventType::Woke;
        RoundTrip("EnemyEvent(Woke)", w, MsgType::EnemyEvent);
        w.type = EnemyEventType::CustomAttack;
        w.arg = 1;
        RoundTrip("EnemyEvent(CustomAttack)", w, MsgType::EnemyEvent);
        std::vector<uint8_t> bad = Encode(w);
        w.type = EnemyEventType::Interacted;
        RoundTrip("EnemyEvent(Interacted)", w, MsgType::EnemyEvent);
        bad[9] = static_cast<uint8_t>(EnemyEventType::Interacted) + 1;   // type byte follows [u8 msg][u64 id]
        MsgEnemyEvent r;
        CHECK(!Decode(bad.data(), bad.size(), r));
    }

    {
        MsgEnemyBlob eb;
        eb.enemyId = 0x1122334455667788ull;
        eb.ownerSlot = 2;
        eb.layout = 0xCAFEF00Du;
        for (int i = 0; i < 300; ++i) eb.data.push_back(static_cast<uint8_t>(i * 7));
        RoundTrip("EnemyBlob", eb, MsgType::EnemyBlob);
        RoundTrip("EnemyBlob(empty)", MsgEnemyBlob{}, MsgType::EnemyBlob);
        MsgEnemyMontage em;
        em.enemyId = 0x99AABBCCDDEEFF00ull;
        em.ownerSlot = 1;
        em.rate = 1.25f;
        em.path = "/Game/Enemies/WhaleKing/Anims/WhaleKing_Bite_Mid_Montage.WhaleKing_Bite_Mid_Montage";
        RoundTrip("EnemyMontage", em, MsgType::EnemyMontage);
        RoundTrip("EnemyMontage(stop)", MsgEnemyMontage{}, MsgType::EnemyMontage);
        std::vector<uint8_t> b = Encode(eb);
        MsgEnemyBlob r;
        b.pop_back();
        CHECK(!Decode(b.data(), b.size(), r));              // truncated
        b = Encode(eb);
        b[9] = static_cast<uint8_t>(kMaxPlayers);
        CHECK(!Decode(b.data(), b.size(), r));              // bad slot
    }

    MsgDamageRequest dr;
    dr.enemyId = 99;
    dr.damage = 123.75f;
    dr.damageTags = {"Damage.Type.Slash", "Damage.Element.Fire"};
    dr.dataTags = {{"Data.Crit", 1.5f}, {"Data.Poise", -20.f}};
    dr.sourceGuid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    dr.attackerSlot = 2;
    dr.flags = 0;
    RoundTrip("DamageRequest", dr, MsgType::DamageRequest);

    MsgDamageRequest notify;   // "slot X hit Y": no tags, HitNotifyOnly
    notify.enemyId = 0xABCDEF0123456789ull;
    notify.damage = 40.f;
    notify.attackerSlot = 1;
    notify.flags = kDamageHitNotifyOnly;
    RoundTrip("DamageRequest(notify)", notify, MsgType::DamageRequest);

    CHECK(kProtocolVersion == 5);   // v5: EnemyBlob / Interacted
    CHECK(static_cast<uint8_t>(MsgType::EnemyOwner) == 45);
    MsgEnemyOwner eo;
    eo.enemyId = 0x0123456789ABCDEFull;
    eo.ownerSlot = 3;
    eo.hp = 412.5f;
    eo.maxHp = 560.f;
    eo.pos = {-1520.25f, 33000.5f, 87.f};
    eo.yaw = -91.5f;
    RoundTrip("EnemyOwner", eo, MsgType::EnemyOwner);
    CHECK(Encode(eo).size() == 34);

    MsgWorldFlags wf;
    wf.full = true;
    wf.flags = {{"GR_Door_01", 1}, {"LIB_Lever_3", -7}, {"Boss_Queen_Dead", 2147483647}};
    RoundTrip("WorldFlags", wf, MsgType::WorldFlags);

    MsgInteractRequest ir;
    ir.requestId = 0xDEADBEEF;
    ir.actorPath = "/Game/Maps/100_GR/100_GR.100_GR:PersistentLevel.Lever_C_3";
    RoundTrip("InteractRequest", ir, MsgType::InteractRequest);

    MsgInteractResult ires;
    ires.requestId = 0xDEADBEEF;
    ires.outcome = InteractOutcome::DoneByHost;
    RoundTrip("InteractResult", ires, MsgType::InteractResult);

    MsgHpScale hs;
    hs.normal = 1.12f;
    hs.elite = 1.18f;
    hs.boss = 1.25f;
    hs.playerCount = 2;
    RoundTrip("HpScale", hs, MsgType::HpScale);

    MsgSummon su;
    su.pos = {-1.f, -2.f, -3.f};
    su.yaw = 90.f;
    su.areaName = "300_LIB";
    RoundTrip("Summon", su, MsgType::Summon);

    // Range checks.
    {
        std::vector<uint8_t> b = Encode(reject);
        MsgReject            r;
        b[1] = 0;
        CHECK(!Decode(b.data(), b.size(), r));
        b[1] = 6;
        CHECK(!Decode(b.data(), b.size(), r));
        b[1] = 5;
        CHECK(Decode(b.data(), b.size(), r) && r.reason == RejectReason::Kicked);
    }
    {
        std::vector<uint8_t> b = Encode(left);
        MsgPeerLeft          r;
        b[1] = kMaxPlayers;  // slot out of range
        CHECK(!Decode(b.data(), b.size(), r));
    }
    {
        std::vector<uint8_t> b = Encode(hello);
        b.back() = 2;  // SaveMode out of range
        MsgHello r;
        CHECK(!Decode(b.data(), b.size(), r));
    }
    {
        // v3 Hello wire layout: [type][u16 protocol][modName][modAuthor][modVersion][gameBuild][name][saveMode],
        // strings as u16 length + bytes.
        MsgHello h3;
        h3.modName = "KD";
        h3.modAuthor = "M";
        h3.modVersion = "1";
        h3.gameBuild = "b";
        h3.name = "n";
        h3.saveMode = SaveMode::OwnSave;
        const std::vector<uint8_t> b = Encode(h3);
        CHECK(b.size() == 1 + 2 + (2 + 2) + (2 + 1) + (2 + 1) + (2 + 1) + (2 + 1) + 1);
        CHECK(b.size() > 7 && b[5] == 'K' && b[6] == 'D');   // modName right after the protocol
        uint16_t proto = 0;
        CHECK(PeekHelloProtocol(b.data(), b.size(), proto) && proto == kProtocolVersion);
        MsgHello r;
        CHECK(Decode(b.data(), b.size(), r) && r.modName == "KD" && r.modAuthor == "M");
    }
    {
        // A pre-1.0 (protocol 2) Hello: [type][u16 2][modVersion][gameBuild][name][saveMode]. It must not decode as v3,
        // but its protocol number must still be readable so the host can answer Reject(Protocol).
        std::vector<uint8_t> v2 = {static_cast<uint8_t>(MsgType::Hello), 2, 0};
        for (const char* str : {"0.2.0", "20261030-123456789", "Knight"}) {
            const size_t n = std::strlen(str);
            v2.push_back(static_cast<uint8_t>(n));
            v2.push_back(0);
            v2.insert(v2.end(), str, str + n);
        }
        v2.push_back(0);   // SaveMode::OwnSave
        MsgHello r;
        CHECK(!Decode(v2.data(), v2.size(), r));
        uint16_t proto = 0;
        CHECK(PeekHelloProtocol(v2.data(), v2.size(), proto) && proto == 2);
        CHECK(!PeekHelloProtocol(v2.data(), 2, proto));                     // truncated
        const std::vector<uint8_t> rej = Encode(reject);
        CHECK(!PeekHelloProtocol(rej.data(), rej.size(), proto));           // not a Hello
        CHECK(!PeekHelloProtocol(nullptr, 0, proto));
    }
    {
        std::vector<uint8_t> b = Encode(wf);
        b[1] = 2;  // bool must be 0/1
        MsgWorldFlags r;
        CHECK(!Decode(b.data(), b.size(), r));
    }
    {
        MsgPlayerState bad = ps;
        bad.hpPct = 101;
        std::vector<uint8_t> b = Encode(bad);
        MsgPlayerState       r;
        CHECK(!Decode(b.data(), b.size(), r));
    }
    {
        // Wrong type byte.
        std::vector<uint8_t> b = Encode(left);
        MsgNameChanged       r;
        CHECK(!Decode(b.data(), b.size(), r));
    }
    {
        // Equip: slot range, path length limit (Encode cuts at a UTF-8 boundary, Decode rejects > 256 bytes).
        std::vector<uint8_t> b = Encode(eq);
        MsgEquip             r;
        b[1] = kMaxPlayers;
        CHECK(!Decode(b.data(), b.size(), r));

        MsgEquip longPath;
        longPath.slot = 2;
        longPath.itemPath = "/Game/" + Repeat(kCjk, 100);   // 6 + 300 bytes
        b = Encode(longPath);
        CHECK(Decode(b.data(), b.size(), r));
        CHECK(r.itemPath.size() <= kMaxItemPathBytes);
        CHECK(r.itemPath.size() == 255);                      // 6 + 83*3: the 84th 春 would straddle byte 256
        CHECK(IsValidUtf8(r.itemPath));

        ByteWriter w;
        w.U8(static_cast<uint8_t>(MsgType::Equip));
        w.U8(0);
        const std::string tooLong(kMaxItemPathBytes + 1, 'x');
        w.U16(static_cast<uint16_t>(tooLong.size()));
        w.Bytes(tooLong.data(), tooLong.size());
        CHECK(!Decode(w.Buffer().data(), w.Size(), r));

        const std::string exact(kMaxItemPathBytes, 'y');
        b = Encode(MsgEquip{0, exact});
        CHECK(Decode(b.data(), b.size(), r) && r.itemPath == exact);

        const std::vector<uint8_t> eqBytes = Encode(eq);
        MsgPlayerEvent             wrong;
        CHECK(!Decode(eqBytes.data(), eqBytes.size(), wrong));   // an Equip is not a PlayerEvent
    }
    {
        // Slot fields: EnemyStates.ownerSlot (byte 3, after type + seq), EnemyEvent.ownerSlot (last byte),
        // DamageRequest.attackerSlot (second to last) + flags (last, only bit0 allowed), EnemyOwner.ownerSlot (byte 9).
        std::vector<uint8_t> b = Encode(es);
        MsgEnemyStates       rs;
        CHECK(b[3] == 2);
        b[3] = kMaxPlayers;
        CHECK(!Decode(b.data(), b.size(), rs));
        b[3] = kMaxPlayers - 1;
        CHECK(Decode(b.data(), b.size(), rs) && rs.ownerSlot == kMaxPlayers - 1);

        b = Encode(ee);
        MsgEnemyEvent re;
        CHECK(b.back() == 3);
        b.back() = kMaxPlayers;
        CHECK(!Decode(b.data(), b.size(), re));
        b.back() = 0;
        CHECK(Decode(b.data(), b.size(), re) && re.ownerSlot == 0);

        b = Encode(dr);
        MsgDamageRequest rd;
        CHECK(b[b.size() - 2] == 2 && b.back() == 0);
        b[b.size() - 2] = kMaxPlayers;
        CHECK(!Decode(b.data(), b.size(), rd));
        b[b.size() - 2] = 1;
        b.back() = 4;   // unknown flag bit
        CHECK(!Decode(b.data(), b.size(), rd));
        b.back() = kDamageHitNotifyOnly;
        CHECK(Decode(b.data(), b.size(), rd) && rd.attackerSlot == 1 && rd.flags == kDamageHitNotifyOnly);

        b = Encode(eo);
        MsgEnemyOwner ro;
        CHECK(b[9] == 3);
        b[9] = kMaxPlayers;
        CHECK(!Decode(b.data(), b.size(), ro));
        b[9] = 0;
        CHECK(Decode(b.data(), b.size(), ro) && ro.ownerSlot == 0);

        const std::vector<uint8_t> eoBytes = Encode(eo);
        MsgEnemyEvent              wrongEv;
        CHECK(!Decode(eoBytes.data(), eoBytes.size(), wrongEv));   // an EnemyOwner is not an EnemyEvent
    }
    CHECK(PeekType(nullptr, 0) == static_cast<MsgType>(0));
}

// ------------------------------------------------------------------ 2. names

void TestNameTruncation() {
    Section("2. UTF-8 name truncation");

    const std::string exact = Repeat(kCjk, 24);  // 72 bytes == kMaxNameBytes
    CHECK(exact.size() == kMaxNameBytes);

    MsgHello h;
    h.name = Repeat(kCjk, 25);  // 75 bytes
    MsgHello back;
    std::vector<uint8_t> b = Encode(h);
    CHECK(Decode(b.data(), b.size(), back));
    CHECK(back.name == exact);

    // 1 + 24*3 = 73 bytes: the cut at 72 lands inside the 24th 春, which must be dropped whole.
    MsgNameChanged n;
    n.slot = 0;
    n.name = "a" + Repeat(kCjk, 24);
    MsgNameChanged nb;
    b = Encode(n);
    CHECK(Decode(b.data(), b.size(), nb));
    CHECK(nb.name == "a" + Repeat(kCjk, 23));
    CHECK(nb.name.size() == 70);
    CHECK(IsValidUtf8(nb.name));

    // "ab" + 24 春 = 74 bytes: cut at 72 lands after the first byte of the 24th 春.
    n.name = "ab" + Repeat(kCjk, 24);
    b = Encode(n);
    CHECK(Decode(b.data(), b.size(), nb));
    CHECK(nb.name == "ab" + Repeat(kCjk, 23));
    CHECK(IsValidUtf8(nb.name));

    PeerEntry e{1, 5, "xyz" + Repeat(kCjk, 30)};
    MsgPeerJoined pj{e};
    MsgPeerJoined pjb;
    b = Encode(pj);
    CHECK(Decode(b.data(), b.size(), pjb));
    CHECK(pjb.peer.name.size() <= kMaxNameBytes);
    CHECK(IsValidUtf8(pjb.peer.name));

    CHECK(Utf8Truncate(kCjk, 1).empty());
    CHECK(Utf8Truncate(kCjk, 2).empty());
    CHECK(Utf8Truncate(kCjk, 3) == kCjk);

    // A peer that sends an over-long name is rejected.
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MsgType::NameChanged));
    w.U8(0);
    const std::string tooLong(kMaxNameBytes + 1, 'x');
    w.U16(static_cast<uint16_t>(tooLong.size()));
    w.Bytes(tooLong.data(), tooLong.size());
    CHECK(!Decode(w.Buffer().data(), w.Size(), nb));
}

// ------------------------------------------------------------------ 3. EncodeSplit

void TestEncodeSplit() {
    Section("3. EncodeSplit");
    MsgEnemyStates big;
    big.seq = 4242;
    big.ownerSlot = 3;
    for (uint64_t i = 0; i < 200; ++i) {
        EnemyState e;
        e.enemyId = i;
        e.hp = static_cast<float>(i);
        e.maxHp = 200.f;
        e.targetSlot = static_cast<uint8_t>(i % 5 == 4 ? 0xFF : i % 4);
        big.enemies.push_back(e);
    }
    const auto chunks = EncodeSplit(big);
    std::printf("  200 enemies -> %zu chunks\n", chunks.size());
    CHECK(chunks.size() >= 2);
    size_t   total = 0;
    uint64_t nextId = 0;
    bool     orderOk = true, sizeOk = true, decodeOk = true, seqOk = true;
    for (const auto& c : chunks) {
        if (c.size() > 1100) sizeOk = false;
        MsgEnemyStates part;
        if (!Decode(c.data(), c.size(), part)) {
            decodeOk = false;
            continue;
        }
        if (part.seq != big.seq || part.ownerSlot != big.ownerSlot) seqOk = false;
        for (const auto& e : part.enemies) {
            if (e.enemyId != nextId++ || !Eq(e, big.enemies[e.enemyId])) orderOk = false;
        }
        total += part.enemies.size();
    }
    CHECK(sizeOk);
    CHECK(decodeOk);
    CHECK(seqOk);
    CHECK(orderOk);
    CHECK(total == 200);

    const auto empty = EncodeSplit(MsgEnemyStates{});
    CHECK(empty.size() == 1);

    const auto small = EncodeSplit(big, 300);
    bool smallOk = true;
    for (const auto& c : small)
        if (c.size() > 300) smallOk = false;
    CHECK(smallOk);
}

// ------------------------------------------------------------------ 4. UDP loopback

constexpr uint16_t kUdpTestPort = 27999;

// Sends `count` numbered reliable messages from an ephemeral-port transport to one on kUdpTestPort.
bool RunUdpReliable(double dropRate, int count, size_t payloadBytes) {
    auto a = CreateUdpTransport(0);
    auto b = CreateUdpTransport(kUdpTestPort);
    const bool initA = a->Init();
    const bool initB = b->Init();
    CHECK(initA);
    CHECK(initB);
    if (!initA || !initB) return false;
    SetUdpTestDropRate(a.get(), dropRate, 2026);

    const PeerId bId = MakeUdpPeerId("127.0.0.1", kUdpTestPort);
    a->OpenPeer(bId);

    std::vector<uint8_t> msg(payloadBytes < 4 ? 4 : payloadBytes);
    for (int i = 0; i < count; ++i) {
        std::memcpy(msg.data(), &i, 4);
        for (size_t k = 4; k < msg.size(); ++k) msg[k] = static_cast<uint8_t>(i + k);
        CHECK(a->Send(bId, Channel::Reliable, msg.data(), msg.size()));
    }

    std::vector<Packet> pa, pb;
    int                 received = 0;
    bool                orderOk = true, contentOk = true, fromOk = true;
    const auto          deadline = Clock::now() + std::chrono::seconds(60);
    while (received < count && Clock::now() < deadline) {
        a->Poll(pa);
        pa.clear();
        b->Poll(pb);
        for (const Packet& p : pb) {
            if (p.channel != Channel::Reliable) continue;
            if (p.from != a->LocalId()) fromOk = false;
            if (p.data.size() != msg.size()) {
                contentOk = false;
                ++received;
                continue;
            }
            int idx = -1;
            std::memcpy(&idx, p.data.data(), 4);
            if (idx != received) orderOk = false;
            for (size_t k = 4; k < p.data.size(); ++k)
                if (p.data[k] != static_cast<uint8_t>(idx + k)) contentOk = false;
            ++received;
        }
        pb.clear();
        SleepMs(1);
    }
    std::printf("  drop %.0f%%, %d x %zu bytes: received %d, ping %d ms\n", dropRate * 100, count, msg.size(), received,
                a->PingMs(bId));
    CHECK(received == count);
    CHECK(orderOk);
    CHECK(contentOk);
    CHECK(fromOk);
    a->Shutdown();
    b->Shutdown();
    return received == count && orderOk && contentOk;
}

void TestUdp() {
    Section("4. UDP loopback");
    CHECK(MakeUdpPeerId("127.0.0.1", 27777) == (0x7F00'0000'0000'0000ull | (0x7F000001ull << 16) | 27777));
    CHECK(MakeUdpPeerId("localhost", 1) == MakeUdpPeerId("127.0.0.1", 1));
    CHECK(MakeUdpPeerId("256.0.0.1", 1) == kInvalidPeer);
    CHECK(MakeUdpPeerId("1.2.3", 1) == kInvalidPeer);
    CHECK(MakeUdpPeerId("1.2.3.4.5", 1) == kInvalidPeer);
    CHECK(MakeUdpPeerId("1.2.3.4", 0) == kInvalidPeer);

    RunUdpReliable(0.0, 500, 4);
    RunUdpReliable(0.2, 500, 4);
    RunUdpReliable(0.2, 20, 20000);  // fragmented reliable messages under loss

    // Unreliable: delivered, size limit enforced, unknown sender accepted (host learns clients this way).
    auto a = CreateUdpTransport(0);
    auto b = CreateUdpTransport(kUdpTestPort);
    CHECK(a->Init());
    CHECK(b->Init());
    const PeerId bId = MakeUdpPeerId("127.0.0.1", kUdpTestPort);
    a->OpenPeer(bId);
    const char hi[] = "hello";
    CHECK(a->Send(bId, Channel::Unreliable, hi, sizeof(hi)));
    std::vector<uint8_t> tooBig(1101);
    CHECK(!a->Send(bId, Channel::Unreliable, tooBig.data(), tooBig.size()));
    CHECK(!a->Send(MakeUdpPeerId("127.0.0.1", 1), Channel::Unreliable, hi, sizeof(hi)));  // never opened
    std::vector<Packet> pb;
    for (int i = 0; i < 200 && pb.empty(); ++i) {
        b->Poll(pb);
        SleepMs(1);
    }
    CHECK(pb.size() == 1);
    if (!pb.empty()) {
        CHECK(pb[0].channel == Channel::Unreliable);
        CHECK(pb[0].from == a->LocalId());
        CHECK(pb[0].data.size() == sizeof(hi) && std::memcmp(pb[0].data.data(), hi, sizeof(hi)) == 0);
        // b never called OpenPeer(a) but can answer it.
        CHECK(b->Send(pb[0].from, Channel::Unreliable, hi, sizeof(hi)));
    }
    a->Shutdown();
    b->Shutdown();
}

// ------------------------------------------------------------------ 5. Session over UDP

void TestSessionUdp() {
    Section("5. Session over UDP");
    constexpr uint16_t kHostPort = 27998;

    SessionConfig hc;
    hc.kind = TransportKind::Udp;
    hc.modVersion = "test";
    hc.gameBuild = "test";
    hc.displayName = "host";
    hc.udpListenPort = kHostPort;
    hc.timeoutMs = 1000;
    SessionConfig cc = hc;
    cc.displayName = "client";
    cc.udpListenPort = 0;

    Session host, client;
    PeerId  hostSaw = kInvalidPeer, clientSaw = kInvalidPeer;
    int     hostPackets = 0, clientDisconnects = 0;
    std::vector<SessionState> clientStates;
    host.OnPeerConnected = [&](PeerId p) { hostSaw = p; };
    client.OnPeerConnected = [&](PeerId p) { clientSaw = p; };
    host.OnPacket = [&](const Packet& p) {
        if (PeekType(p.data.data(), p.data.size()) == MsgType::Heartbeat) std::printf("  heartbeat leaked to OnPacket\n");
        ++hostPackets;
    };
    client.OnPeerDisconnected = [&](PeerId) { ++clientDisconnects; };
    client.OnStateChanged = [&](SessionState s) { clientStates.push_back(s); };

    CHECK(host.Init(hc));
    CHECK(client.Init(cc));
    host.Host();
    CHECK(host.State() == SessionState::Hosting);
    CHECK(host.IsHost());
    client.JoinUdp("127.0.0.1", kHostPort);
    CHECK(client.State() == SessionState::Connecting);
    CHECK(!client.IsHost());

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while ((hostSaw == kInvalidPeer || clientSaw == kInvalidPeer) && Clock::now() < deadline) {
        host.Tick(0.016f);
        client.Tick(0.016f);
        SleepMs(5);
    }
    CHECK(hostSaw != kInvalidPeer);
    CHECK(clientSaw == MakeUdpPeerId("127.0.0.1", kHostPort));
    CHECK(client.State() == SessionState::Connected);
    CHECK(client.HostId() == MakeUdpPeerId("127.0.0.1", kHostPort));
    CHECK(host.Peers().size() == 1);
    CHECK(client.Peers().size() == 1);
    CHECK(!clientStates.empty() && clientStates.back() == SessionState::Connected);

    // A game message goes through; heartbeats never reach OnPacket.
    MsgPeerLeft probe;
    probe.slot = 1;
    const auto bytes = Encode(probe);
    CHECK(client.Send(client.HostId(), Channel::Reliable, bytes.data(), bytes.size()));
    const auto until = Clock::now() + std::chrono::milliseconds(1500);  // long enough for at least one heartbeat
    while (Clock::now() < until) {
        host.Tick(0.016f);
        client.Tick(0.016f);
        SleepMs(5);
    }
    CHECK(hostPackets == 1);

    // Host goes silent (stops ticking AND its keepalive thread is muted, i.e. the process is really gone): the client
    // must give up after timeoutMs. (With keepalive on, a stalled host stays alive — see TestSessionKeepalive.)
    SessionTestAccess::SetKeepaliveEnabled(host, false);
    const auto failDeadline = Clock::now() + std::chrono::seconds(4);
    while (client.State() != SessionState::Failed && Clock::now() < failDeadline) {
        client.Tick(0.016f);
        SleepMs(10);
    }
    CHECK(client.State() == SessionState::Failed);
    CHECK(client.LastError() == "host lost");
    CHECK(clientDisconnects == 1);
    CHECK(client.Peers().empty());

    client.Leave();
    CHECK(client.State() == SessionState::Idle);
    host.Leave();
    CHECK(host.State() == SessionState::Idle);
    host.Shutdown();
    client.Shutdown();
}

// ------------------------------------------------------------------ 6. keepalive while the game thread stalls

void TestSessionKeepalive() {
    Section("6. Session keepalive (host game thread stalled 5 s)");
    constexpr uint16_t kHostPort = 27998;

    CHECK(SessionConfig{}.timeoutMs == 10000);

    SessionConfig hc;
    hc.kind = TransportKind::Udp;
    hc.modVersion = "test";
    hc.gameBuild = "test";
    hc.displayName = "host";
    hc.udpListenPort = kHostPort;
    hc.timeoutMs = 3000;   // shorter than the 5 s stall: without keepalive the client WOULD time out
    SessionConfig cc = hc;
    cc.displayName = "client";
    cc.udpListenPort = 0;

    Session host, client;
    PeerId  hostSaw = kInvalidPeer, clientSaw = kInvalidPeer;
    int     hostDisconnects = 0, clientDisconnects = 0;
    host.OnPeerConnected = [&](PeerId p) { hostSaw = p; };
    client.OnPeerConnected = [&](PeerId p) { clientSaw = p; };
    host.OnPeerDisconnected = [&](PeerId) { ++hostDisconnects; };
    client.OnPeerDisconnected = [&](PeerId) { ++clientDisconnects; };

    CHECK(host.Init(hc));
    CHECK(client.Init(cc));
    host.Host();
    client.JoinUdp("127.0.0.1", kHostPort);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while ((hostSaw == kInvalidPeer || clientSaw == kInvalidPeer) && Clock::now() < deadline) {
        host.Tick(0.016f);
        client.Tick(0.016f);
        SleepMs(5);
    }
    CHECK(hostSaw != kInvalidPeer);
    CHECK(client.State() == SessionState::Connected);

    // The host's "game thread" freezes for 5 s (only the client ticks). After 1.5 s the host's keepalive thread starts
    // sending heartbeats every 250 ms, so the client must stay connected.
    const uint32_t hostKaBefore = SessionTestAccess::KeepaliveHeartbeats(host);
    const auto     stallEnd = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < stallEnd) {
        client.Tick(0.016f);
        SleepMs(10);
    }
    const uint32_t hostKa = SessionTestAccess::KeepaliveHeartbeats(host) - hostKaBefore;
    std::printf("  keepalive heartbeats sent by the stalled host: %u (expected ~14)\n", hostKa);
    CHECK(hostKa >= 8);
    CHECK(client.State() == SessionState::Connected);
    CHECK(clientDisconnects == 0);
    CHECK(client.Peers().size() == 1);

    // The host wakes up: it drains the client's buffered heartbeats before judging timeouts, so nobody is dropped.
    const auto resumeEnd = Clock::now() + std::chrono::milliseconds(1200);
    while (Clock::now() < resumeEnd) {
        host.Tick(0.016f);
        client.Tick(0.016f);
        SleepMs(5);
    }
    CHECK(hostDisconnects == 0);
    CHECK(host.Peers().size() == 1);
    CHECK(client.State() == SessionState::Connected);
    CHECK(SessionTestAccess::KeepaliveHeartbeats(client) == 0);   // the client never stalled

    // Keepalive never resurrects a host that really died: mute it, keep the host stalled, the client times out.
    SessionTestAccess::SetKeepaliveEnabled(host, false);
    const auto failDeadline = Clock::now() + std::chrono::seconds(6);
    while (client.State() != SessionState::Failed && Clock::now() < failDeadline) {
        client.Tick(0.016f);
        SleepMs(10);
    }
    CHECK(client.State() == SessionState::Failed);
    CHECK(clientDisconnects == 1);

    client.Leave();
    host.Leave();
    host.Shutdown();
    client.Shutdown();
    CHECK(host.State() == SessionState::Idle);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestRoundTrips();
    TestNameTruncation();
    TestEncodeSplit();
    TestUdp();
    TestSessionUdp();
    TestSessionKeepalive();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
