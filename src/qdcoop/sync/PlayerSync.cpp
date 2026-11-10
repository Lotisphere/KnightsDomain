// QD Coop — player state sync. See PlayerSync.h.
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/FS_ItemData_classes.hpp"
#include "SDK/FS_PlayerChar_classes.hpp"
#include "SDK/FS_PlayerChar_parameters.hpp"
#include "SDK/WeaponScriptBase_classes.hpp"

#include "qdcoop/sync/PlayerSync.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/Puppet.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qd::sync {
namespace {

constexpr const char* kTag = "playersync";

constexpr float  kSendInterval     = 0.05f;    // 20 Hz
constexpr double kInterpDelay      = 0.100;    // render remote players 100 ms in the past
constexpr double kMaxExtrapolate   = 0.250;
constexpr float  kSnapDistance     = 1000.0f;  // cm: larger jumps are not interpolated
constexpr size_t kBufferMax        = 32;
constexpr double kStaleSec         = 2.0;      // no snapshot for this long -> remove the puppet
constexpr double kAttackWindow     = 0.35;     // LastAttackTime changed within this window -> actionId 1
constexpr double kMaxHpRefreshSec  = 1.0;
constexpr double kSpawnRetrySec    = 3.0;
constexpr float  kDefaultHalfHeight = 90.0f;
constexpr float  kLabelHeight      = 200.0f;   // above the puppet's feet
constexpr float  kLabelFadeStart   = 2500.0f;
constexpr float  kLabelMaxDist     = 4000.0f;
constexpr float  kDummyDistance    = 300.0f;
constexpr float  kEquipPollSec     = 0.25f;    // how often the local in-hand weapon is read
constexpr unsigned kHostColor  = 0xFF78D7FFu;  // IM_COL32(255,215,120,255)
constexpr unsigned kOtherColor = 0xFFFFFFFFu;  // IM_COL32(255,255,255,255)
constexpr unsigned kDummyColor = 0xFFC3A7F2u;  // IM_COL32(242,167,195,255) — accent pink

using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

float NormalizeAxis(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0) a += 360.0f;
    return a - 180.0f;
}
float LerpAngle(float a, float b, float t) { return NormalizeAxis(a + NormalizeAxis(b - a) * t); }
float Dist(const net::Vec3f& a, const net::Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Snap {
    double t = 0;   // local receive time
    net::MsgPlayerState s;
};

struct Remote {
    std::deque<Snap>        buf;
    bool                    hasSeq = false;
    uint16_t                lastSeq = 0;
    std::unique_ptr<Puppet> puppet;
    double                  nextSpawnTry = 0;
    // held weapon
    bool                    hasEquip = false;   // a MsgEquip arrived for this member
    std::string             equipPath;          // its item path ("" = empty-handed)
    bool                    poseValid = false;  // RemotePose(): set every Tick
    net::Vec3f              poseFeet{};
    float                   poseYaw = 0;
};

struct LocalEquip {             // the local player's in-hand weapon
    float       accum = 0;
    bool        known = false;  // read at least once in the game world
    std::string path;
    bool        warnedLong = false;
};

struct LocalState {
    float          sendAccum = 0;
    uint16_t       seq = 0;
    SDK::ACharacter* ch = nullptr;
    bool           isFsChar = false;
    bool           primed = false;          // edge detectors have a previous value
    float          prevAttackTime = 0;
    double         lastAttackChange = -100;
    uint16_t       prevAction = 0;
    bool           prevDead = false;
    bool           prevTeleporting = false;
    float          maxHp = 0;
    double         maxHpAt = -100;
    SDK::UFunction* getMaxHp = nullptr;
    SDK::UWorld*   areaWorld = nullptr;
    uint16_t       areaId = 0;
};

struct Sync {
    std::array<Remote, net::kMaxPlayers> remotes;
    LocalState  local;
    LocalEquip  equip;
    SDK::UWorld* world = nullptr;
    float       halfHeight = kDefaultHalfHeight;
    bool        halfHeightKnown = false;
    gamethread::TickerId ticker = 0;
    std::unique_ptr<Puppet> dummy;
    double      dummyNextTry = 0;
};
Sync& S() {
    static Sync s;
    return s;
}

std::atomic<int>      g_statPuppets{0}, g_statRemotes{0}, g_statArea{0};
std::atomic<float>    g_statHalfHeight{0};
std::atomic<bool>     g_dummyOn{false};
std::atomic<uint16_t> g_dummyAction{0};
std::atomic<float>    g_dummySpeed{0};
std::atomic<bool>     g_dummyAttackPulse{false};

// ------------------------------------------------------------------ local

uint16_t AreaIdFor(SDK::UWorld* world) {
    auto& L = S().local;
    if (world != L.areaWorld) {
        L.areaWorld = world;
        L.areaId = world ? static_cast<uint16_t>(ue::Fnv1a64(ue::Name(world)) & 0xFFFF) : 0;
        QD_DEBUG(kTag, "area id for world {} = {:04X}", world ? ue::Name(world) : std::string("<none>"), L.areaId);
    }
    return L.areaId;
}

float QueryMaxHp(SDK::AFS_PlayerChar_C* pc) {
    auto& L = S().local;
    if (!L.getMaxHp) L.getMaxHp = ue::FindFunction(pc, "GetMaxHP");
    if (!L.getMaxHp) return 0;
    SDK::Params::FS_PlayerChar_C_GetMaxHP parms{};
    parms.Recache = false;
    ue::CallFunction(pc, L.getMaxHp, &parms);
    return parms.HP;
}

void SendEvent(net::PlayerEventType type, uint8_t slot, const net::Vec3f& pos, int32_t arg = 0) {
    net::MsgPlayerEvent e;
    e.slot = slot;
    e.type = type;
    e.arg = arg;
    e.pos = pos;
    coop::Broadcast(net::Channel::Reliable, net::Encode(e));
}

// ------------------------------------------------------------------ held weapon

// MsgPlayerState::weaponId: quick-compare hash of the item path; 0 = empty hands.
uint16_t WeaponIdFor(const std::string& itemPath) {
    if (itemPath.empty()) return 0;
    const auto id = static_cast<uint16_t>(ue::Fnv1a64(itemPath) & 0xFFFF);
    return id != 0 ? id : uint16_t{1};
}

// FS_PlayerChar_C::CurrentInHandWeapon -> ItemData -> asset path. `ok` = false when the character is not an
// FS_PlayerChar_C (nothing meaningful to report); "" with ok = true means empty hands.
std::string ReadLocalWeaponPath(SDK::ACharacter* ch, bool& ok) {
    ok = false;
    if (!ch->IsA(SDK::AFS_PlayerChar_C::StaticClass())) return {};
    ok = true;
    SDK::UWeaponScriptBase_C* weapon = static_cast<SDK::AFS_PlayerChar_C*>(ch)->CurrentInHandWeapon;
    if (!weapon || !ue::IsValid(weapon)) return {};
    SDK::UFS_ItemData_C* item = weapon->ItemData;
    if (!item || !ue::IsValid(item)) return {};
    std::string path = ue::PathName(item);
    if (path.size() > net::kMaxItemPathBytes) {
        auto& e = S().equip;
        if (!e.warnedLong) {
            QD_WARN(kTag, "weapon item path is longer than {} bytes, sending empty hands: {}", net::kMaxItemPathBytes, path);
            e.warnedLong = true;
        }
        return {};
    }
    return path;
}

// Our current weapon -> one member (host: direct; client: via the host, which relays) or everyone (toSlot empty).
void SendEquip(std::optional<uint8_t> toSlot) {
    auto& e = S().equip;
    auto slot = coop::LocalSlot();
    if (!e.known || !slot || !coop::InSession()) return;
    net::MsgEquip m;
    m.slot = *slot;
    m.itemPath = e.path;
    const auto bytes = net::Encode(m);
    if (toSlot && coop::IsHost())
        coop::SendToSlot(*toSlot, net::Channel::Reliable, bytes);
    else
        coop::Broadcast(net::Channel::Reliable, bytes);
}

void ApplyRemoteEquip(uint8_t slot, const std::string& path) {
    auto& r = S().remotes[slot];
    if (r.hasEquip && r.equipPath == path) return;
    r.hasEquip = true;
    r.equipPath = path;
    QD_INFO(kTag, "slot {} weapon -> {}", slot, path.empty() ? std::string("<empty hands>") : path);
    if (r.puppet) r.puppet->SetWeapon(path);
}

void OnEquip(uint8_t fromSlot, const uint8_t* data, size_t size) {
    net::MsgEquip e;
    if (!net::Decode(data, size, e)) {
        QD_DEBUG(kTag, "bad Equip from slot {}", fromSlot);
        return;
    }
    if (coop::IsHost()) {
        // From a client (Coop gives us its verified slot). Never trust the payload slot; relay to the other clients,
        // exactly like Coop relays NameChanged.
        if (fromSlot == 0 || fromSlot >= net::kMaxPlayers) return;
        e.slot = fromSlot;
        const auto bytes = net::Encode(e);
        for (const auto& m : coop::Members())
            if (m.present && !m.isLocal && m.slot != fromSlot) coop::SendToSlot(m.slot, net::Channel::Reliable, bytes);
    }
    // Client: everything comes from the host (Coop passes fromSlot 0), the payload slot names the original sender.
    if (e.slot >= net::kMaxPlayers) return;
    if (auto local = coop::LocalSlot(); local && *local == e.slot) return;
    ApplyRemoteEquip(e.slot, e.itemPath);
}

// Every 0.25 s while in the game world: read the in-hand weapon; on change, tell everyone (and the debug fit menu).
void PollLocalEquip(float dt, SDK::ACharacter* ch, bool inSession) {
    auto& e = S().equip;
    e.accum += dt;
    if (e.known && e.accum < kEquipPollSec) return;
    e.accum = 0;
    bool              ok = false;
    const std::string path = ReadLocalWeaponPath(ch, ok);
    if (!ok || (e.known && path == e.path)) return;
    e.known = true;
    e.path = path;
    QD_INFO(kTag, "local weapon -> {}", path.empty() ? std::string("<empty hands>") : path);
    SetLocalWeaponPath(path);
    if (inSession) SendEquip(std::nullopt);
}

// ------------------------------------------------------------------ local state

void SampleAndSend(double now, SDK::ACharacter* ch) {
    auto& L = S().local;
    auto slot = coop::LocalSlot();
    if (!slot) return;

    if (ch != L.ch) {
        L.ch = ch;
        L.isFsChar = ch->IsA(SDK::AFS_PlayerChar_C::StaticClass());
        L.primed = false;
        L.maxHpAt = -100;
        L.getMaxHp = nullptr;
        if (!L.isFsChar) QD_WARN(kTag, "local character is a {}, not FS_PlayerChar_C", ue::ClassName(ch));
    }

    net::MsgPlayerState s;
    s.slot = *slot;
    s.seq = ++L.seq;

    const SDK::FVector loc = ch->K2_GetActorLocation();
    s.pos = {loc.X, loc.Y, loc.Z};

    SDK::APlayerController* pc = ue::LocalController();
    SDK::FRotator rot = (pc && ue::IsValid(pc)) ? pc->ControlRotation : ch->K2_GetActorRotation();
    s.yaw = NormalizeAxis(rot.Yaw);
    s.pitch = NormalizeAxis(rot.Pitch);

    uint8_t moveMode = 0;
    if (SDK::UCharacterMovementComponent* cm = ch->CharacterMovement) {
        s.vel = {cm->Velocity.X, cm->Velocity.Y, cm->Velocity.Z};
        moveMode = static_cast<uint8_t>(cm->MovementMode);
    }
    s.moveMode = moveMode;

    bool dead = false, teleporting = false, dashing = false;
    s.hpPct = 100;
    if (L.isFsChar) {
        auto* p = static_cast<SDK::AFS_PlayerChar_C*>(ch);
        dead = p->IsDead;
        teleporting = p->Teleporting;
        dashing = p->Dashing;

        const float attackTime = p->LastAttackTime;
        if (!L.primed) L.prevAttackTime = attackTime;
        else if (attackTime != L.prevAttackTime) {
            L.prevAttackTime = attackTime;
            L.lastAttackChange = now;
        }

        if (now - L.maxHpAt > kMaxHpRefreshSec) {
            L.maxHpAt = now;
            L.maxHp = QueryMaxHp(p);
        }
        const float maxHp = L.maxHp > 0 ? L.maxHp : p->BaseHealth;
        if (maxHp > 0) {
            const float pct = std::round(p->Health / maxHp * 100.0f);
            s.hpPct = static_cast<uint8_t>(std::clamp(pct, 0.0f, 100.0f));
        }
    }

    const bool falling = moveMode == static_cast<uint8_t>(SDK::EMovementMode::MOVE_Falling);
    uint16_t action = 0;
    if (dead)                                         action = 3;
    else if (teleporting)                             action = 5;
    else if (now - L.lastAttackChange < kAttackWindow) action = 1;
    else if (dashing)                                 action = 2;
    else if (falling)                                 action = 4;
    s.actionId = action;
    s.weaponId = WeaponIdFor(S().equip.path);   // quick-compare only; the path itself travels in MsgEquip
    s.areaId = AreaIdFor(ue::World());

    coop::Broadcast(net::Channel::Unreliable, net::Encode(s));

    if (L.primed) {
        if (action == 1 && L.prevAction != 1)    SendEvent(net::PlayerEventType::Attack, *slot, s.pos);
        if (dead && !L.prevDead)                 SendEvent(net::PlayerEventType::Died, *slot, s.pos);
        if (!dead && L.prevDead)                 SendEvent(net::PlayerEventType::Respawned, *slot, s.pos);
        if (teleporting && !L.prevTeleporting)   SendEvent(net::PlayerEventType::Teleported, *slot, s.pos);
    }
    L.prevAction = action;
    L.prevDead = dead;
    L.prevTeleporting = teleporting;
    L.primed = true;
}

// ------------------------------------------------------------------ remote

struct Interp {
    net::Vec3f pos{}, vel{};
    float      yaw = 0;
    uint16_t   action = 0;
    uint16_t   area = 0;
};

void Fill(Interp& out, const net::MsgPlayerState& s) {
    out.pos = s.pos;
    out.vel = s.vel;
    out.yaw = s.yaw;
    out.action = s.actionId;
    out.area = s.areaId;
}

bool Interpolate(const std::deque<Snap>& buf, double t, Interp& out) {
    if (buf.empty()) return false;
    if (t <= buf.front().t) {
        Fill(out, buf.front().s);
        return true;
    }
    const Snap& last = buf.back();
    if (t >= last.t) {
        Fill(out, last.s);
        if (last.s.actionId != 3 && last.s.actionId != 5) {   // never extrapolate the dead or teleporting
            const float ex = static_cast<float>(std::min(t - last.t, kMaxExtrapolate));
            out.pos.x += last.s.vel.x * ex;
            out.pos.y += last.s.vel.y * ex;
            out.pos.z += last.s.vel.z * ex;
        }
        return true;
    }
    for (size_t i = buf.size() - 1; i > 0; --i) {
        const Snap& a = buf[i - 1];
        const Snap& b = buf[i];
        if (t < a.t) continue;
        const double span = b.t - a.t;
        const float alpha = span > 1e-6 ? static_cast<float>((t - a.t) / span) : 1.0f;
        if (Dist(a.s.pos, b.s.pos) > kSnapDistance) {   // teleport: jump, don't slide across the map
            Fill(out, alpha < 0.5f ? a.s : b.s);
            return true;
        }
        Fill(out, a.s);
        out.pos.x = a.s.pos.x + (b.s.pos.x - a.s.pos.x) * alpha;
        out.pos.y = a.s.pos.y + (b.s.pos.y - a.s.pos.y) * alpha;
        out.pos.z = a.s.pos.z + (b.s.pos.z - a.s.pos.z) * alpha;
        out.vel.x = a.s.vel.x + (b.s.vel.x - a.s.vel.x) * alpha;
        out.vel.y = a.s.vel.y + (b.s.vel.y - a.s.vel.y) * alpha;
        out.vel.z = a.s.vel.z + (b.s.vel.z - a.s.vel.z) * alpha;
        out.yaw = LerpAngle(a.s.yaw, b.s.yaw, alpha);
        return true;
    }
    Fill(out, buf.front().s);
    return true;
}

void ClearRemote(uint8_t slot) {
    if (slot >= net::kMaxPlayers) return;
    auto& r = S().remotes[slot];
    if (r.puppet) r.puppet->Destroy();
    r = Remote{};
}

void ClearAllRemotes() {
    for (uint8_t i = 0; i < net::kMaxPlayers; ++i) ClearRemote(i);
}

void OnPlayerState(uint8_t fromSlot, const uint8_t* data, size_t size) {
    if (fromSlot >= net::kMaxPlayers) return;
    if (auto local = coop::LocalSlot(); local && *local == fromSlot) return;
    net::MsgPlayerState s;
    if (!net::Decode(data, size, s)) return;

    auto& r = S().remotes[fromSlot];
    if (r.hasSeq && static_cast<int16_t>(static_cast<uint16_t>(s.seq - r.lastSeq)) <= 0) return;   // old / duplicate
    r.hasSeq = true;
    r.lastSeq = s.seq;
    r.buf.push_back({Now(), s});
    while (r.buf.size() > kBufferMax) r.buf.pop_front();
}

void OnPlayerEvent(uint8_t fromSlot, const uint8_t* data, size_t size) {
    if (fromSlot >= net::kMaxPlayers) return;
    net::MsgPlayerEvent e;
    if (!net::Decode(data, size, e)) return;
    auto& r = S().remotes[fromSlot];
    switch (e.type) {
        case net::PlayerEventType::Attack:
            if (r.puppet) r.puppet->TriggerAttack();
            break;
        case net::PlayerEventType::Died:
        case net::PlayerEventType::Respawned:
        case net::PlayerEventType::Teleported:
            QD_DEBUG(kTag, "slot {} event {}", fromSlot, static_cast<int>(e.type));
            break;   // death/respawn/teleport are reflected by actionId + the snap rule
        default:
            break;
    }
}

// ------------------------------------------------------------------ labels

struct LabelCtx {
    SDK::APlayerController* pc = nullptr;
    SDK::FVector camLoc;
    int vw = 0, vh = 0;
};

void AddLabel(std::vector<ui::WorldLabel>& out, const LabelCtx& ctx, const net::Vec3f& feet, const std::string& text,
              unsigned color) {
    if (!ctx.pc) return;
    const SDK::FVector head(feet.x, feet.y, feet.z + kLabelHeight);
    const float dx = head.X - ctx.camLoc.X, dy = head.Y - ctx.camLoc.Y, dz = head.Z - ctx.camLoc.Z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist > kLabelMaxDist) return;
    const float alpha = dist <= kLabelFadeStart ? 1.0f : 1.0f - (dist - kLabelFadeStart) / (kLabelMaxDist - kLabelFadeStart);

    SDK::FVector2D scr;
    if (!ctx.pc->ProjectWorldLocationToScreen(head, &scr, false)) return;   // false => behind the camera
    if (scr.X < 0 || scr.Y < 0) return;
    if (ctx.vw > 0 && scr.X > static_cast<float>(ctx.vw)) return;
    if (ctx.vh > 0 && scr.Y > static_cast<float>(ctx.vh)) return;

    ui::WorldLabel l;
    l.screenX = scr.X;
    l.screenY = scr.Y;
    l.alpha = std::clamp(alpha, 0.0f, 1.0f);
    l.text = text;
    l.color = color;
    out.push_back(std::move(l));
}

// ------------------------------------------------------------------ tick

void Tick(float dt) {
    auto& s = S();
    const double now = Now();

    // World switch: the old world's actors are going away with it; forget them without touching them.
    SDK::UWorld* world = ue::World();
    if (world != s.world) {
        for (auto& r : s.remotes)
            if (r.puppet) r.puppet->Abandon(), r.puppet.reset();
        if (s.dummy) s.dummy->Abandon(), s.dummy.reset();
        s.world = world;
        s.halfHeightKnown = false;
        QD_INFO(kTag, "world changed -> {}", world ? ue::Name(world) : std::string("<none>"));
    }

    SDK::ACharacter* ch = ue::LocalCharacter();
    const bool inGame = ch && ue::IsValid(ch);

    if (inGame && !s.halfHeightKnown && ch->CapsuleComponent && ue::IsValid(ch->CapsuleComponent)) {
        const float hh = ch->CapsuleComponent->GetScaledCapsuleHalfHeight();
        if (hh > 1.0f) {
            s.halfHeight = hh;
            s.halfHeightKnown = true;
            g_statHalfHeight = hh;
            QD_INFO(kTag, "player capsule half height = {:.1f}", hh);
        }
    }

    // ---- local -> network
    const bool inSession = coop::InSession();
    if (inGame) PollLocalEquip(dt, ch, inSession);   // also solo: feeds the debug dummy and the fit sliders
    if (inSession) {
        s.local.sendAccum += dt;
        if (s.local.sendAccum >= kSendInterval) {
            s.local.sendAccum = std::fmod(s.local.sendAccum, kSendInterval);
            if (inGame) SampleAndSend(now, ch);
            else s.local.primed = false;
        }
    } else {
        s.local.sendAccum = 0;
        s.local.primed = false;
    }
    const uint16_t localArea = inGame ? AreaIdFor(world) : 0;
    g_statArea = localArea;

    // ---- label context
    LabelCtx lc;
    if (inGame) {
        SDK::APlayerController* pc = ue::LocalController();
        if (pc && ue::IsValid(pc)) {
            lc.pc = pc;
            if (pc->PlayerCameraManager && ue::IsValid(pc->PlayerCameraManager))
                lc.camLoc = pc->PlayerCameraManager->GetCameraLocation();
            else
                lc.camLoc = ch->K2_GetActorLocation();
            pc->GetViewportSize(&lc.vw, &lc.vh);
        }
    }
    std::vector<ui::WorldLabel> labels;

    // ---- network -> puppets
    const auto& members = coop::Members();
    int puppets = 0, remotes = 0;
    for (uint8_t slot = 0; slot < net::kMaxPlayers; ++slot) {
        auto& r = s.remotes[slot];
        const coop::Member& m = members[slot];
        r.poseValid = false;
        if (!m.present || m.isLocal) {
            if (r.puppet || !r.buf.empty() || r.hasEquip) ClearRemote(slot);
            continue;
        }
        Interp ip;
        bool want = inGame && inSession && !r.buf.empty() && (now - r.buf.back().t) < kStaleSec;
        if (want) {
            ++remotes;
            want = Interpolate(r.buf, now - kInterpDelay, ip) && ip.area == localArea;
        }
        if (!want) {
            if (r.puppet) {
                r.puppet->Destroy();
                r.puppet.reset();
            }
            continue;
        }

        const net::Vec3f feet{ip.pos.x, ip.pos.y, ip.pos.z - s.halfHeight};
        r.poseValid = true;   // RemotePose(): valid even if the puppet below fails to spawn
        r.poseFeet = feet;
        r.poseYaw = ip.yaw;
        if (!r.puppet) {
            r.puppet = std::make_unique<Puppet>(slot);
            if (r.hasEquip) r.puppet->SetWeapon(r.equipPath);   // applied on Spawn
        }
        if (!r.puppet->IsSpawned()) {
            if (now < r.nextSpawnTry) continue;
            if (!r.puppet->Spawn(feet, ip.yaw)) {
                r.nextSpawnTry = now + kSpawnRetrySec;
                continue;
            }
        }
        r.puppet->SetTransform(feet, ip.yaw);
        const float speed2D = std::sqrt(ip.vel.x * ip.vel.x + ip.vel.y * ip.vel.y);
        r.puppet->Update(ip.action, speed2D, dt);
        ++puppets;

        const std::string name = m.name.empty() ? ui::Tr("name.default") + std::to_string(slot + 1) : m.name;
        AddLabel(labels, lc, feet, name, slot == 0 ? kHostColor : kOtherColor);
    }

    // ---- debug dummy
    if (g_dummyOn && inGame) {
        if (!s.dummy) s.dummy = std::make_unique<Puppet>(0xFF);
        if (s.equip.known) s.dummy->SetWeapon(s.equip.path);   // the dummy mirrors our own weapon (no-op if same)
        const SDK::FVector loc = ch->K2_GetActorLocation();
        float yaw = lc.pc ? lc.pc->ControlRotation.Yaw : ch->K2_GetActorRotation().Yaw;
        const float rad = yaw * 3.14159265f / 180.0f;
        const net::Vec3f feet{loc.X + std::cos(rad) * kDummyDistance, loc.Y + std::sin(rad) * kDummyDistance,
                              loc.Z - s.halfHeight};
        const float faceYaw = NormalizeAxis(yaw + 180.0f);
        bool ok = s.dummy->IsSpawned();
        if (!ok && now >= s.dummyNextTry) {
            ok = s.dummy->Spawn(feet, faceYaw);
            if (!ok) s.dummyNextTry = now + kSpawnRetrySec;
        }
        if (ok) {
            s.dummy->SetTransform(feet, faceYaw);
            if (g_dummyAttackPulse.exchange(false)) s.dummy->TriggerAttack();
            s.dummy->Update(g_dummyAction.load(), g_dummySpeed.load(), dt);
            AddLabel(labels, lc, feet, "测试化身", kDummyColor);
        }
    } else if (s.dummy) {
        s.dummy->Destroy();
        s.dummy.reset();
    }

    g_statPuppets = puppets;
    g_statRemotes = remotes;
    ui::PublishWorldLabels(std::move(labels));
}

}  // namespace

void PlayerSyncInit() {
    auto& s = S();
    if (s.ticker) return;
    coop::On(net::MsgType::PlayerState, OnPlayerState);
    coop::On(net::MsgType::PlayerEvent, OnPlayerEvent);
    coop::On(net::MsgType::Equip, OnEquip);
    coop::AddMemberListener([](const coop::Member& m, bool joined) {
        if (m.isLocal) return;
        ClearRemote(m.slot);                  // fresh buffer on join, puppet removed on leave
        if (joined) SendEquip(m.slot);        // the newcomer learns what we are holding
    });
    // OnSessionEnded() is a single slot: chain whatever was there before.
    auto prev = coop::OnSessionEnded();
    coop::OnSessionEnded() = [prev]() {
        if (prev) prev();
        ClearAllRemotes();
    };
    s.ticker = gamethread::AddTicker("playersync", [](float dt) { Tick(dt); });
    QD_INFO(kTag, "initialized");
}

void PlayerSyncShutdown() {
    auto& s = S();
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    ClearAllRemotes();
    if (s.dummy) {
        s.dummy->Destroy();
        s.dummy.reset();
    }
    ui::PublishWorldLabels({});
}

PlayerSyncStats GetPlayerSyncStats() {
    PlayerSyncStats st;
    st.puppets = g_statPuppets;
    st.remotes = g_statRemotes;
    st.localAreaId = static_cast<uint16_t>(g_statArea.load());
    st.capsuleHalfHeight = g_statHalfHeight;
    return st;
}

void DebugSetDummy(bool enabled) { g_dummyOn = enabled; }

void DebugSetDummyAction(uint16_t actionId, float speed2D) {
    if (actionId == 1) {   // attack is a one-shot: pulse it, then keep idling
        g_dummyAttackPulse = true;
        g_dummyAction = 0;
        g_dummySpeed = 0;
        return;
    }
    g_dummyAction = actionId;
    g_dummySpeed = speed2D;
}

bool DebugDummyEnabled() { return g_dummyOn; }
uint16_t DebugDummyAction() { return g_dummyAction; }
float DebugDummySpeed() { return g_dummySpeed; }

}  // namespace qd::sync

// ================================================================== enemy sync queries

namespace qd::playersync {

SDK::AActor* PuppetActor(uint8_t slot) {
    if (slot >= net::kMaxPlayers) return nullptr;
    const auto& r = sync::S().remotes[slot];
    return r.puppet ? r.puppet->Actor() : nullptr;
}

// Dev tooling: make a remote puppet swing right now, without waiting for the network edge.
void DebugTriggerAttack(uint8_t slot) {
    if (slot >= net::kMaxPlayers) return;
    auto& r = sync::S().remotes[slot];
    if (r.puppet) r.puppet->TriggerAttack();
}

bool RemotePose(uint8_t slot, net::Vec3f& feet, float& yaw) {
    if (slot >= net::kMaxPlayers) return false;
    const auto& r = sync::S().remotes[slot];
    if (!r.poseValid) return false;
    feet = r.poseFeet;
    yaw = r.poseYaw;
    return true;
}

}  // namespace qd::playersync
