// Knights' Domain — co-op hub: Session lifecycle, handshake, slot roster, message routing, local display name.
// Hello carries modName/modAuthor (checked by the host), Steam ownership gate (core/Ownership.h) between Session
// init and the first Session::Tick, all player-facing text through ui::Tr.
// Game thread only (Session callbacks fire inside Session::Tick, which runs from our ticker).
#include "qdcoop/sync/Coop.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Ownership.h"
#include "qdcoop/sync/CoopInternal.h"
#include "qdcoop/sync/GameQueries.h"
#include "qdcoop/sync/Profiles.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include <Windows.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace qd::coop {
namespace {

constexpr const char* kTag = "coop";
constexpr float kHelloResendSec   = 1.0f;
constexpr int   kHelloMaxAttempts = 15;     // ~15 s without a Welcome -> give up
constexpr float kHelloWaitSec     = 10.0f;  // host: close peers that never say Hello
constexpr float kRejectCloseSec   = 2.0f;
constexpr float kNameCheckSec     = 2.0f;

struct PendingClose {
    net::PeerId peer = net::kInvalidPeer;
    float       remaining = 0;
};

struct Hub {
    std::unique_ptr<net::Session> session;
    bool sessionOk = false;
    bool udp = false;
    net::SessionConfig sessionConfig;
    float initRetryTimer = 0;
    float initElapsed = 0;
    bool  initGaveUp = false;
    bool  devAutoDone = false;
    bool  coopDisabled = false;     // ownership check failed (or Steam never came up): no session at all

    std::array<Member, net::kMaxPlayers> members{};
    uint8_t localSlot = 0;          // valid when host/idle (always 0) or client after Welcome
    bool    welcomed = false;       // client: Welcome received for the current connection

    std::unordered_map<uint8_t, std::vector<Handler>> handlers;
    std::function<void()> onStarted, onEnded;
    std::vector<std::function<void(const Member&, bool)>> memberListeners;
    bool wasInSession = false;

    std::string localName;
    std::string profileKey;
    int         saveSlot = 0;
    float       nameTimer = 0;

    gamethread::TickerId ticker = 0;

    // client handshake
    bool  helloActive = false;
    float helloTimer = 0;
    int   helloAttempts = 0;
    bool  hostLostHandled = false;

    // host bookkeeping
    std::vector<PendingClose> pendingCloses;
    std::unordered_map<net::PeerId, float> awaitingHello;   // peer -> seconds waited

    bool leaveRequested = false;
};

Hub& H() {
    static Hub h;
    return h;
}

bool IsClientState(net::SessionState s) {
    return s == net::SessionState::JoiningLobby || s == net::SessionState::Connecting || s == net::SessionState::Connected;
}

net::SessionState CurState() {
    return H().sessionOk ? H().session->State() : net::SessionState::Idle;
}

const char* RejectText(net::RejectReason r) {
    switch (r) {
        case net::RejectReason::Full:       return ui::Tr("reject.full");
        case net::RejectReason::ModVersion: return ui::Tr("reject.mod");
        case net::RejectReason::GameBuild:  return ui::Tr("reject.game");
        case net::RejectReason::Protocol:   return ui::Tr("reject.protocol");
        case net::RejectReason::Kicked:     return ui::Tr("reject.kicked");
    }
    return ui::Tr("reject.unknown");
}

std::string DefaultName() {
#if QD_DEV
    if (H().udp) return "Player" + std::to_string(config::Get().profile);
#endif
    std::string persona = profiles::SteamPersonaName();
    return persona.empty() ? std::string(ui::Tr("name.default")) : persona;
}

std::string ComputeProfileKey() {
    const auto& cfg = config::Get();
    std::string dev = "dev-profile-" + std::to_string(cfg.profile);
    if (H().udp || !H().sessionOk) return dev;
    const net::PeerId id = H().session->LocalId();
    return id != net::kInvalidPeer ? std::to_string(id) : dev;
}

std::string NameOr(const std::string& name, uint8_t slot) {
    std::string n = profiles::Sanitize(name);
    return n.empty() ? std::string(ui::Tr("name.default")) + std::to_string(slot + 1) : n;
}

Member MakeLocal(uint8_t slot) {
    Member m;
    m.present = true;
    m.slot = slot;
    m.peer = H().sessionOk ? H().session->LocalId() : net::kInvalidPeer;
    m.name = H().localName;
    m.isLocal = true;
    return m;
}

void NotifyMember(const Member& m, bool joined) {
    auto listeners = H().memberListeners;   // copy: listeners may register more listeners
    for (auto& fn : listeners)
        if (fn) fn(m, joined);
}

// Clears every remote member (optionally notifying listeners) and puts the local player in slot 0.
void ResetRosterToSolo(bool notify) {
    auto& h = H();
    std::vector<Member> removed;
    for (auto& m : h.members)
        if (m.present && !m.isLocal) removed.push_back(m);
    h.members = {};
    h.localSlot = 0;
    h.welcomed = false;
    h.members[0] = MakeLocal(0);
    if (notify)
        for (auto& m : removed) NotifyMember(m, false);
}

bool HasRemoteMembers() {
    for (auto& m : H().members)
        if (m.present && !m.isLocal) return true;
    return false;
}

void SendBytes(net::PeerId to, net::Channel ch, const std::vector<uint8_t>& bytes) {
    if (to == net::kInvalidPeer || bytes.empty() || !H().sessionOk) return;
    if (!H().session->Send(to, ch, bytes.data(), bytes.size()))
        QD_DEBUG(kTag, "send of {} bytes to {:016X} rejected", bytes.size(), to);
}

// Host only: send to every remote member except `exceptSlot`.
void RelayToClients(uint8_t exceptSlot, net::Channel ch, const std::vector<uint8_t>& bytes) {
    for (auto& m : H().members)
        if (m.present && !m.isLocal && m.slot != exceptSlot) SendBytes(m.peer, ch, bytes);
}

void Dispatch(net::MsgType type, uint8_t fromSlot, const uint8_t* data, size_t size) {
    auto it = H().handlers.find(static_cast<uint8_t>(type));
    if (it == H().handlers.end()) return;
    auto list = it->second;   // copy: a handler may register another handler
    for (auto& fn : list)
        if (fn) fn(fromSlot, data, size);
}

std::optional<uint8_t> FreeSlot() {
    for (uint8_t s = 1; s < net::kMaxPlayers; ++s)
        if (!H().members[s].present) return s;
    return std::nullopt;
}

// ------------------------------------------------------------------ host side

void SendWelcome(net::PeerId to, uint8_t slot) {
    net::MsgWelcome w;
    w.yourSlot = slot;
    for (auto& m : H().members) {
        if (!m.present) continue;
        net::PeerEntry e;
        e.slot = m.slot;
        e.peerId = m.peer;
        e.name = m.name;
        w.peers.push_back(std::move(e));
    }
    SendBytes(to, net::Channel::Reliable, net::Encode(w));
}

void HostReject(net::PeerId peer, net::RejectReason reason) {
    net::MsgReject r;
    r.reason = reason;
    SendBytes(peer, net::Channel::Reliable, net::Encode(r));
    H().pendingCloses.push_back({peer, kRejectCloseSec});
    H().awaitingHello.erase(peer);
    QD_INFO(kTag, "rejected {:016X}: {}", peer, static_cast<int>(reason));
    ui::Toast(ui::TrF("toast.rejected_join", RejectText(reason)), ui::ToastKind::Warning);
}

void HostOnHello(net::PeerId from, const uint8_t* data, size_t size) {
    auto& h = H();
    net::MsgHello hello;
    if (!net::Decode(data, size, hello)) {
        // A Hello of another protocol version (e.g. a pre-1.0 v2 client) does not decode; answer it with
        // Reject(Protocol) instead of letting the client time out, if its version field is readable.
        uint16_t proto = 0;
        if (net::PeekHelloProtocol(data, size, proto) && proto != net::kProtocolVersion && !SlotOf(from)) {
            for (auto& pc : H().pendingCloses)
                if (pc.peer == from) return;   // already rejected, waiting to close
            QD_INFO(kTag, "Hello from {:016X} uses protocol {} (ours {})", from, proto, net::kProtocolVersion);
            return HostReject(from, net::RejectReason::Protocol);
        }
        QD_WARN(kTag, "bad Hello from {:016X}", from);
        return;
    }
    if (auto existing = SlotOf(from)) {   // duplicate Hello (client resends until Welcome arrives)
        SendWelcome(from, *existing);
        return;
    }
    for (auto& pc : h.pendingCloses)
        if (pc.peer == from) return;      // already rejected, waiting to close

    if (hello.protocol != net::kProtocolVersion) return HostReject(from, net::RejectReason::Protocol);
    if (hello.modName != core::kModName || hello.modAuthor != core::kModAuthor) {
        QD_WARN(kTag, "Hello from {:016X}: mod '{}' by '{}' (expected '{}' by '{}')", from, hello.modName,
                hello.modAuthor, core::kModName, core::kModAuthor);
        return HostReject(from, net::RejectReason::ModVersion);
    }
    if (hello.modVersion != core::kModVersion)   return HostReject(from, net::RejectReason::ModVersion);
    if (hello.gameBuild != core::GameBuild())    return HostReject(from, net::RejectReason::GameBuild);
    auto slot = FreeSlot();
    if (!slot) return HostReject(from, net::RejectReason::Full);

    Member m;
    m.present = true;
    m.slot = *slot;
    m.peer = from;
    m.name = NameOr(hello.name, *slot);
    m.isLocal = false;
    h.members[*slot] = m;
    h.awaitingHello.erase(from);

    SendWelcome(from, *slot);

    net::MsgPeerJoined pj;
    pj.peer.slot = m.slot;
    pj.peer.peerId = m.peer;
    pj.peer.name = m.name;
    RelayToClients(m.slot, net::Channel::Reliable, net::Encode(pj));

    QD_INFO(kTag, "peer {:016X} joined as slot {} \"{}\"", from, m.slot, m.name);
    ui::Toast(ui::TrF("toast.joined", m.name), ui::ToastKind::Success);
    NotifyMember(m, true);
}

void HostRemovePeer(net::PeerId peer) {
    auto& h = H();
    h.awaitingHello.erase(peer);
    auto slot = SlotOf(peer);
    if (!slot || *slot == h.localSlot) return;
    Member gone = h.members[*slot];
    h.members[*slot] = {};

    net::MsgPeerLeft pl;
    pl.slot = gone.slot;
    RelayToClients(gone.slot, net::Channel::Reliable, net::Encode(pl));

    QD_INFO(kTag, "slot {} \"{}\" left", gone.slot, gone.name);
    ui::Toast(ui::TrF("toast.left", gone.name), ui::ToastKind::Info);
    NotifyMember(gone, false);
}

void HostOnPacket(const net::Packet& p, net::MsgType type) {
    auto& h = H();
    if (type == net::MsgType::Hello) return HostOnHello(p.from, p.data.data(), p.data.size());

    auto fromSlot = SlotOf(p.from);
    if (!fromSlot || *fromSlot == h.localSlot) return;   // not (yet) a member: ignore everything but Hello
    const uint8_t slot = *fromSlot;

    switch (type) {
        case net::MsgType::PlayerState: {
            net::MsgPlayerState s;
            if (!net::Decode(p.data.data(), p.data.size(), s)) return;
            if (s.slot != slot) {   // never trust the slot a client claims
                s.slot = slot;
                auto fixed = net::Encode(s);
                RelayToClients(slot, p.channel, fixed);
                Dispatch(type, slot, fixed.data(), fixed.size());
            } else {
                RelayToClients(slot, p.channel, p.data);
                Dispatch(type, slot, p.data.data(), p.data.size());
            }
            return;
        }
        case net::MsgType::PlayerEvent: {
            net::MsgPlayerEvent e;
            if (!net::Decode(p.data.data(), p.data.size(), e)) return;
            if (e.slot != slot) {
                e.slot = slot;
                auto fixed = net::Encode(e);
                RelayToClients(slot, p.channel, fixed);
                Dispatch(type, slot, fixed.data(), fixed.size());
            } else {
                RelayToClients(slot, p.channel, p.data);
                Dispatch(type, slot, p.data.data(), p.data.size());
            }
            return;
        }
        case net::MsgType::NameChanged: {
            net::MsgNameChanged n;
            if (!net::Decode(p.data.data(), p.data.size(), n)) return;
            n.slot = slot;
            n.name = NameOr(n.name, slot);
            const std::string old = h.members[slot].name;
            h.members[slot].name = n.name;
            auto bytes = net::Encode(n);
            RelayToClients(slot, net::Channel::Reliable, bytes);
            if (old != n.name) ui::Toast(ui::TrF("toast.renamed", old, n.name), ui::ToastKind::Info);
            Dispatch(type, slot, bytes.data(), bytes.size());
            return;
        }
        case net::MsgType::Welcome:
        case net::MsgType::Reject:
        case net::MsgType::PeerJoined:
        case net::MsgType::PeerLeft:
            return;   // host-only messages coming from a client: ignore
        default:
            Dispatch(type, slot, p.data.data(), p.data.size());
            return;
    }
}

// ------------------------------------------------------------------ client side

void SendHello() {
    auto& h = H();
    const net::PeerId host = h.session->HostId();
    if (host == net::kInvalidPeer) return;
    net::MsgHello m;
    m.protocol = net::kProtocolVersion;
    m.modName = core::kModName;
    m.modAuthor = core::kModAuthor;
    m.modVersion = core::kModVersion;
    m.gameBuild = core::GameBuild();
    m.name = h.localName;
    m.saveMode = net::SaveMode::OwnSave;
    SendBytes(host, net::Channel::Reliable, net::Encode(m));
    ++h.helloAttempts;
    QD_DEBUG(kTag, "Hello #{} -> host {:016X}", h.helloAttempts, host);
}

void ClientHostLost() {
    auto& h = H();
    if (h.hostLostHandled) return;
    h.hostLostHandled = true;
    const bool wasJoined = h.welcomed;
    h.helloActive = false;
    ResetRosterToSolo(true);
    if (wasJoined) ui::Toast(ui::Tr("toast.host_left"), ui::ToastKind::Warning, 6.0f);
    h.leaveRequested = true;
}

void ClientOnWelcome(const net::Packet& p) {
    auto& h = H();
    net::MsgWelcome w;
    if (!net::Decode(p.data.data(), p.data.size(), w)) {
        QD_WARN(kTag, "bad Welcome");
        return;
    }
    if (h.welcomed) return;   // duplicate (we resend Hello until the first Welcome lands)
    if (w.yourSlot == 0 || w.yourSlot >= net::kMaxPlayers) {
        QD_WARN(kTag, "Welcome with invalid slot {}", w.yourSlot);
        return;
    }
    h.members = {};
    h.localSlot = w.yourSlot;
    h.welcomed = true;
    h.helloActive = false;
    h.members[w.yourSlot] = MakeLocal(w.yourSlot);

    std::string hostName = ui::Tr("toast.host_default");
    std::vector<Member> joined;
    for (auto& e : w.peers) {
        if (e.slot >= net::kMaxPlayers || e.slot == w.yourSlot) continue;
        Member m;
        m.present = true;
        m.slot = e.slot;
        m.peer = e.slot == 0 ? h.session->HostId() : e.peerId;
        m.name = NameOr(e.name, e.slot);
        m.isLocal = false;
        h.members[e.slot] = m;
        if (e.slot == 0) hostName = m.name;
        joined.push_back(m);
    }
    QD_INFO(kTag, "welcomed as slot {} ({} other members)", w.yourSlot, joined.size());
    ui::Toast(ui::TrF("toast.joined_room", hostName), ui::ToastKind::Success);
    for (auto& m : joined) NotifyMember(m, true);
}

void ClientOnPacket(const net::Packet& p, net::MsgType type) {
    auto& h = H();
    const net::PeerId host = h.session->HostId();
    if (host != net::kInvalidPeer && p.from != host) return;   // clients only talk to the host

    switch (type) {
        case net::MsgType::Welcome:
            ClientOnWelcome(p);
            return;
        case net::MsgType::Reject: {
            net::MsgReject r;
            if (!net::Decode(p.data.data(), p.data.size(), r)) return;
            QD_INFO(kTag, "rejected by host: {}", static_cast<int>(r.reason));
            ui::Toast(ui::TrF("toast.cannot_join", RejectText(r.reason)), ui::ToastKind::Error, 8.0f);
            h.helloActive = false;
            h.hostLostHandled = true;   // no "房主已离开" toast after a reject
            h.leaveRequested = true;
            return;
        }
        default:
            break;
    }
    if (!h.welcomed) return;   // everything else needs a slot

    switch (type) {
        case net::MsgType::PeerJoined: {
            net::MsgPeerJoined pj;
            if (!net::Decode(p.data.data(), p.data.size(), pj)) return;
            const uint8_t s = pj.peer.slot;
            if (s == 0 || s >= net::kMaxPlayers || s == h.localSlot) return;
            Member m;
            m.present = true;
            m.slot = s;
            m.peer = pj.peer.peerId;
            m.name = NameOr(pj.peer.name, s);
            h.members[s] = m;
            ui::Toast(ui::TrF("toast.joined", m.name), ui::ToastKind::Success);
            NotifyMember(m, true);
            return;
        }
        case net::MsgType::PeerLeft: {
            net::MsgPeerLeft pl;
            if (!net::Decode(p.data.data(), p.data.size(), pl)) return;
            if (pl.slot == 0 || pl.slot >= net::kMaxPlayers || pl.slot == h.localSlot) return;
            if (!h.members[pl.slot].present) return;
            Member gone = h.members[pl.slot];
            h.members[pl.slot] = {};
            ui::Toast(ui::TrF("toast.left", gone.name), ui::ToastKind::Info);
            NotifyMember(gone, false);
            return;
        }
        case net::MsgType::NameChanged: {
            net::MsgNameChanged n;
            if (!net::Decode(p.data.data(), p.data.size(), n)) return;
            if (n.slot >= net::kMaxPlayers || n.slot == h.localSlot || !h.members[n.slot].present) return;
            const std::string old = h.members[n.slot].name;
            h.members[n.slot].name = NameOr(n.name, n.slot);
            if (old != h.members[n.slot].name)
                ui::Toast(ui::TrF("toast.renamed", old, h.members[n.slot].name), ui::ToastKind::Info);
            Dispatch(type, n.slot, p.data.data(), p.data.size());
            return;
        }
        case net::MsgType::PlayerState: {
            net::MsgPlayerState s;
            if (!net::Decode(p.data.data(), p.data.size(), s)) return;
            if (s.slot >= net::kMaxPlayers || s.slot == h.localSlot) return;
            Dispatch(type, s.slot, p.data.data(), p.data.size());
            return;
        }
        case net::MsgType::PlayerEvent: {
            net::MsgPlayerEvent e;
            if (!net::Decode(p.data.data(), p.data.size(), e)) return;
            if (e.slot >= net::kMaxPlayers || e.slot == h.localSlot) return;
            Dispatch(type, e.slot, p.data.data(), p.data.size());
            return;
        }
        case net::MsgType::Hello:
            return;
        default:
            Dispatch(type, 0, p.data.data(), p.data.size());   // everything else originates at the host
            return;
    }
}

// ------------------------------------------------------------------ session callbacks

void OnPacket(const net::Packet& p) {
    // nothing from the network is processed unless the ownership check passed (Tick() does not even pump the
    // Session before that; this is the second line of defence).
    if (H().coopDisabled || core::OwnershipState() != core::Ownership::Passed) return;
    const net::MsgType type = net::PeekType(p.data.data(), p.data.size());
    if (static_cast<uint8_t>(type) == 0) return;
    if (H().session->IsHost())
        HostOnPacket(p, type);
    else
        ClientOnPacket(p, type);
}

void OnPeerConnected(net::PeerId peer) {
    auto& h = H();
    if (h.session->IsHost()) {
        if (!SlotOf(peer)) h.awaitingHello.emplace(peer, 0.0f);
        QD_INFO(kTag, "peer {:016X} connected, waiting for Hello", peer);
    } else if (peer == h.session->HostId() && !h.welcomed) {
        h.helloActive = true;
        h.helloTimer = 0;   // send right away
    }
}

void OnPeerDisconnected(net::PeerId peer) {
    auto& h = H();
    if (h.session->IsHost()) {
        HostRemovePeer(peer);
    } else if (peer == h.session->HostId()) {
        ClientHostLost();
    }
}

void OnStateChanged(net::SessionState st) {
    auto& h = H();
    QD_INFO(kTag, "session state -> {}", static_cast<int>(st));
    switch (st) {
        case net::SessionState::Hosting:
            ResetRosterToSolo(false);
            h.hostLostHandled = false;
            ui::Toast(ui::Tr("toast.room_created"), ui::ToastKind::Success);
            break;
        case net::SessionState::JoiningLobby:
            ResetRosterToSolo(false);
            h.hostLostHandled = false;
            h.helloAttempts = 0;
            break;
        case net::SessionState::Connecting:
        case net::SessionState::Connected:
            if (!h.welcomed && !h.helloActive) {
                h.helloActive = true;
                h.helloTimer = 0;
                h.helloAttempts = 0;
                h.hostLostHandled = false;
            }
            break;
        case net::SessionState::Failed: {
            const std::string& err = h.session->LastError();
            ui::Toast(ui::TrF("toast.session_failed", err.empty() ? std::string(ui::Tr("common.unknown_error")) : err),
                      ui::ToastKind::Error, 8.0f);
            h.helloActive = false;
            break;
        }
        case net::SessionState::Idle:
            h.helloActive = false;
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ name

void BroadcastLocalName() {
    auto slot = LocalSlot();
    if (!slot || !InSession()) return;
    net::MsgNameChanged n;
    n.slot = *slot;
    n.name = H().localName;
    Broadcast(net::Channel::Reliable, net::Encode(n));
}

void RefreshLocalName(bool broadcast) {
    auto& h = H();
    const int slot = sync::QueryActiveSaveSlot();
    h.saveSlot = slot < 0 ? 0 : slot;
    h.profileKey = ComputeProfileKey();
    std::string name = profiles::GetName(h.profileKey, h.saveSlot).value_or(std::string());
    if (name.empty()) name = DefaultName();
    if (name == h.localName) return;

    QD_INFO(kTag, "local name: \"{}\" ({} slot{})", name, h.profileKey, h.saveSlot);
    h.localName = name;
    for (auto& m : h.members)
        if (m.present && m.isLocal) m.name = name;
    if (broadcast) BroadcastLocalName();
}

// ------------------------------------------------------------------ tick

bool TryInitSession(bool finalAttempt);
#if QD_DEV
void RunDevAutoActions();
#endif

// co-op off for the rest of the run. The Session is shut down, so nothing can be hosted, joined or received.
void DisableCoop(const std::string& why) {
    auto& h = H();
    if (h.coopDisabled) return;
    h.coopDisabled = true;
    h.initGaveUp = true;
    QD_ERROR(kTag, "co-op disabled: {}", why);
    if (h.sessionOk) {
        h.session->Leave();
        h.session->Shutdown();
        h.sessionOk = false;
    }
    h.helloActive = false;
    h.pendingCloses.clear();
    h.awaitingHello.clear();
    ResetRosterToSolo(false);
    ui::Toast(ui::Tr("own.failed"), ui::ToastKind::Error, 15.0f);
}

void Tick(float dt) {
    auto& h = H();
    if (h.coopDisabled) return;
    if (!h.sessionOk) {
        // Steam may come up after we do (the game initializes OnlineSubsystemSteam late): retry for a while.
        h.initRetryTimer -= dt;
        h.initElapsed += dt;
        if (h.initRetryTimer > 0 || h.initGaveUp) return;
        h.initRetryTimer = 2.0f;
        const bool finalAttempt = h.initElapsed >= 90.0f;
        if (!TryInitSession(finalAttempt) && finalAttempt) {
            h.initGaveUp = true;
#if !QD_DEV
            // Release: Steam not ready after 90 s counts as "not launched from Steam".
            core::OwnershipFail("Steam API not ready after 90 s (game not launched from Steam?)");
            DisableCoop(core::OwnershipFailReason());
#endif
        }
        return;
    }
    // no hosting / joining / invites / packets until the ownership check has passed. The Session is not
    // ticked meanwhile, so Steam invites and +connect_lobby wait in its queues.
    switch (core::OwnershipState()) {
        case core::Ownership::Passed:
            break;
        case core::Ownership::Failed:
            DisableCoop(core::OwnershipFailReason());
            return;
        default:
            return;
    }
#if QD_DEV
    RunDevAutoActions();
#endif

    h.session->Tick(dt);
    const net::SessionState st = h.session->State();

    // client: (re)send Hello until the Welcome arrives
    if (h.helloActive && !h.welcomed &&
        (st == net::SessionState::Connecting || st == net::SessionState::Connected)) {
        h.helloTimer -= dt;
        if (h.helloTimer <= 0) {
            if (h.helloAttempts >= kHelloMaxAttempts) {
                h.helloActive = false;
                h.hostLostHandled = true;
                ui::Toast(ui::Tr("toast.host_timeout"), ui::ToastKind::Error, 8.0f);
                h.leaveRequested = true;
            } else {
                SendHello();
                h.helloTimer = kHelloResendSec;
            }
        }
    }

    // host: close rejected peers after the Reject had time to arrive
    if (!h.pendingCloses.empty()) {
        for (auto& pc : h.pendingCloses) pc.remaining -= dt;
        auto it = std::remove_if(h.pendingCloses.begin(), h.pendingCloses.end(), [&](const PendingClose& pc) {
            if (pc.remaining > 0) return false;
            if (auto* t = h.session->Transport()) t->ClosePeer(pc.peer);
            return true;
        });
        h.pendingCloses.erase(it, h.pendingCloses.end());
    }

    // host: drop peers that connected but never sent Hello
    for (auto it = h.awaitingHello.begin(); it != h.awaitingHello.end();) {
        it->second += dt;
        if (it->second > kHelloWaitSec) {
            QD_INFO(kTag, "peer {:016X} never sent Hello, closing", it->first);
            if (auto* t = h.session->Transport()) t->ClosePeer(it->first);
            it = h.awaitingHello.erase(it);
        } else {
            ++it;
        }
    }

    if (h.leaveRequested) {
        h.leaveRequested = false;
        h.session->Leave();
    }

    // Roster hygiene: outside Hosting/Connecting/Connected the roster is just us in slot 0.
    const net::SessionState now = h.session->State();
    const bool live = now == net::SessionState::Hosting || now == net::SessionState::Connecting ||
                      now == net::SessionState::Connected;
    if (!live) {
        if (HasRemoteMembers() || h.welcomed || h.localSlot != 0) ResetRosterToSolo(true);
        h.awaitingHello.clear();
        if (now == net::SessionState::Idle) h.hostLostHandled = false;
    }
    if (now != net::SessionState::Hosting && !h.awaitingHello.empty()) h.awaitingHello.clear();

    // save slot can change on the title screen / after loading: re-resolve the name periodically
    h.nameTimer -= dt;
    if (h.nameTimer <= 0) {
        h.nameTimer = kNameCheckSec;
        RefreshLocalName(true);
    }

    const bool inSession = InSession();
    if (inSession != h.wasInSession) {
        h.wasInSession = inSession;
        QD_INFO(kTag, "co-op session {}", inSession ? "started" : "ended");
        if (inSession) {
            if (h.onStarted) h.onStarted();
        } else {
            if (h.onEnded) h.onEnded();
        }
    }
}

}  // namespace

namespace {

bool TryInitSession(bool finalAttempt) {
    auto& h = H();
    h.sessionOk = Session().Init(h.sessionConfig);
    const char* transport = h.udp ? "udp" : "steam";
    if (h.sessionOk) {
        QD_INFO(kTag, "session ready (transport {}, local id {:016X}) after {:.1f}s", transport,
                Session().LocalId(), h.initElapsed);
        h.members[0].peer = Session().LocalId();
        core::OwnershipCheck();   // dev flavour: skipped, passes at once
        return true;
    }
    Session().Shutdown();   // leave it clean for the next attempt
    if (finalAttempt) {
        QD_ERROR(kTag, "Session::Init failed for {:.0f}s (transport {}); giving up", h.initElapsed, transport);
#if QD_DEV
        ui::Toast(ui::TrF("toast.init_failed", h.udp ? "UDP" : "Steam"), ui::ToastKind::Error, 10.0f);
#endif   // release: Tick() reports it as the ownership failure
    } else if (h.initElapsed == 0) {
        QD_WARN(kTag, "Session::Init not ready yet (transport {}); retrying every 2s", transport);
    }
    return false;
}

#if QD_DEV
// Dev only: "-qdcoop-autohost" hosts, "-qdcoop-autojoin=<ip>:<port>" joins over UDP, once the session is up.
void RunDevAutoActions() {
    auto& h = H();
    if (h.devAutoDone) return;
    h.devAutoDone = true;
    const std::wstring cmd = GetCommandLineW();
    if (cmd.find(L"-qdcoop-autohost") != std::wstring::npos) {
        QD_INFO(kTag, "dev: auto-host");
        h.session->Host();
        return;
    }
    const size_t p = cmd.find(L"-qdcoop-autojoin=");
    if (p != std::wstring::npos && h.udp) {
        std::wstring arg = cmd.substr(p + 17);
        arg = arg.substr(0, arg.find_first_of(L" \t\""));
        const size_t colon = arg.rfind(L':');
        std::string ip(arg.begin(), arg.begin() + (colon == std::wstring::npos ? arg.size() : colon));
        const uint16_t port = colon == std::wstring::npos ? 27777 : static_cast<uint16_t>(std::stoi(arg.substr(colon + 1)));
        QD_INFO(kTag, "dev: auto-join {}:{}", ip, port);
        h.session->JoinUdp(ip, port);
    }
}
#endif   // QD_DEV

}  // namespace

// ================================================================== public contract

void Init() {
    auto& h = H();
    if (h.ticker) return;
    const auto& cfg = config::Get();
#if QD_DEV
    h.udp = cfg.transport == "udp";
#else
    h.udp = false;   // release is Steam-only
#endif

    h.localName = DefaultName();

    net::SessionConfig sc;
    sc.kind = h.udp ? net::TransportKind::Udp : net::TransportKind::Steam;
    sc.maxMembers = net::kMaxPlayers;
    sc.modName = core::kModName;
    sc.modAuthor = core::kModAuthor;
    sc.modVersion = core::kModVersion;
    sc.gameBuild = core::GameBuild();
    sc.displayName = h.localName;   // lobby "host_name": Steam persona (profiles.json names are resolved after Init)
    sc.udpListenPort = static_cast<uint16_t>(cfg.udpPort);

    net::Session& s = Session();
    s.OnPacket = [](const net::Packet& p) { OnPacket(p); };
    s.OnPeerConnected = [](net::PeerId peer) { OnPeerConnected(peer); };
    s.OnPeerDisconnected = [](net::PeerId peer) { OnPeerDisconnected(peer); };
    s.OnStateChanged = [](net::SessionState st) { OnStateChanged(st); };

    h.sessionConfig = sc;
    TryInitSession(false);

    ResetRosterToSolo(false);
    RefreshLocalName(false);
    h.members[0].name = h.localName;

    h.ticker = gamethread::AddTicker("coop", [](float dt) { Tick(dt); });
}

void Shutdown() {
    auto& h = H();
    if (h.ticker) {
        gamethread::RemoveTicker(h.ticker);
        h.ticker = 0;
    }
    if (h.sessionOk) {
        h.session->Leave();
        h.session->Shutdown();
        h.sessionOk = false;
    }
    ResetRosterToSolo(false);
    if (h.wasInSession) {
        h.wasInSession = false;
        if (h.onEnded) h.onEnded();
    }
}

net::Session& Session() {
    auto& h = H();
    if (!h.session) h.session = std::make_unique<net::Session>();
    return *h.session;
}

bool InSession() {
    auto& h = H();
    if (!h.sessionOk) return false;
    const net::SessionState st = h.session->State();
    if (st == net::SessionState::Hosting) return HasRemoteMembers();
    if (st == net::SessionState::Connected) return h.welcomed;
    return false;
}

bool IsHost() {
    return !IsClientState(CurState());
}

int PlayerCount() {
    int n = 0;
    for (auto& m : H().members)
        if (m.present) ++n;
    return n < 1 ? 1 : n;
}

const std::array<Member, net::kMaxPlayers>& Members() {
    return H().members;
}

std::optional<uint8_t> LocalSlot() {
    auto& h = H();
    if (IsClientState(CurState())) {
        if (h.welcomed) return h.localSlot;
        return std::nullopt;
    }
    return static_cast<uint8_t>(0);
}

std::optional<uint8_t> SlotOf(net::PeerId peer) {
    if (peer == net::kInvalidPeer) return std::nullopt;
    for (auto& m : H().members)
        if (m.present && m.peer == peer) return m.slot;
    return std::nullopt;
}

const std::string& LocalName() {
    return H().localName;
}

void SetLocalName(const std::string& utf8) {
    auto& h = H();
    const std::string clean = profiles::Sanitize(utf8);
    if (h.profileKey.empty()) h.profileKey = ComputeProfileKey();
    const bool saved = profiles::SetName(h.profileKey, h.saveSlot, clean);

    std::string name = clean.empty() ? DefaultName() : clean;
    if (name != h.localName) {
        h.localName = name;
        for (auto& m : h.members)
            if (m.present && m.isLocal) m.name = name;
        BroadcastLocalName();
    }
    if (saved)
        ui::Toast(ui::TrF("name.saved", h.localName), ui::ToastKind::Success);
    else
        ui::Toast(ui::Tr("name.save_failed"), ui::ToastKind::Warning);
}

void On(net::MsgType type, Handler handler) {
    if (!handler) return;
    H().handlers[static_cast<uint8_t>(type)].push_back(std::move(handler));
}

void SendToHost(net::Channel ch, const std::vector<uint8_t>& bytes) {
    if (IsHost() || !H().sessionOk) return;
    SendBytes(H().session->HostId(), ch, bytes);
}

void Broadcast(net::Channel ch, const std::vector<uint8_t>& bytes) {
    if (!H().sessionOk) return;
    if (IsHost()) {
        if (H().session->State() != net::SessionState::Hosting) return;
        RelayToClients(H().localSlot, ch, bytes);   // only welcomed members, never half-open peers
    } else {
        SendToHost(ch, bytes);
    }
}

void SendToSlot(uint8_t slot, net::Channel ch, const std::vector<uint8_t>& bytes) {
    auto& h = H();
    if (!h.sessionOk || slot >= net::kMaxPlayers) return;
    const Member& m = h.members[slot];
    if (!m.present || m.isLocal) return;
    if (IsHost()) {
        SendBytes(m.peer, ch, bytes);
    } else if (slot == 0) {
        SendBytes(h.session->HostId(), ch, bytes);
    } else {
        QD_DEBUG(kTag, "SendToSlot({}) from a client: no direct route, dropped", slot);
    }
}

std::function<void()>& OnSessionStarted() { return H().onStarted; }
std::function<void()>& OnSessionEnded() { return H().onEnded; }

void AddMemberListener(std::function<void(const Member&, bool joined)> fn) {
    if (fn) H().memberListeners.push_back(std::move(fn));
}

// ------------------------------------------------------------------ CoopInternal.h

bool SessionReady() { return H().sessionOk; }
bool CoopDisabled() { return H().coopDisabled; }
bool CoopAllowed() {
    return H().sessionOk && !H().coopDisabled && core::OwnershipState() == core::Ownership::Passed;
}
bool UsingUdp() { return H().udp; }
int ActiveSaveSlotCached() { return H().saveSlot; }
const std::string& ProfileKey() { return H().profileKey; }

}  // namespace qd::coop
