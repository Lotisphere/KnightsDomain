// Knights' Domain — Session: Steam lobby / UDP dev hosting, peer liveness, heartbeats.
// lobby data keys kd_mod / kd_author / kd_ver; the release flavour (QD_DEV=0) has no UDP code path at all and
// never references CreateUdpTransport / MakeUdpPeerId (UdpTransport.cpp is not even compiled into that library).
// Knows nothing about the game: it only intercepts MsgType::Heartbeat and forwards every other packet to OnPacket.
//
// Threading: everything here runs on the game thread, except the keepalive thread. Steam callbacks (lobby
// invites, rich-presence joins, lobby member changes) arrive on the OnlineSubsystemSteam thread and are only queued
// (steam::CallbackQueue); Tick() drains them. Steam API-call results (CreateLobby, JoinLobby) are polled with
// IsAPICallCompleted/GetAPICallResult.
//
// Keepalive: a level load blocks the game thread for 10+ s, so Tick() cannot send heartbeats and
// the other side would time us out. Init() starts a thread that wakes every 250 ms; when the last Tick() is more than
// 1.5 s old it sends one Heartbeat to every connected peer. It only sends: it never receives, never judges timeouts and
// never invokes callbacks (timeouts are judged in Tick() alone). The transport is shared between the two threads, so
// every ITransport call goes through LockedTransport (the `io` lock); the peer list the thread uses is a copy refreshed
// at the end of every Tick() under the same lock. It stops covering after 2 min of stall (a hung game should be dropped)
// and never calls into Steam once the game has shut Steam down (quitting also stops Tick()).
//
// Callback timing: OnPeerConnected / OnPeerDisconnected / OnPacket fire inside Tick(). OnStateChanged fires inside
// Tick() too: a state change caused by a call made outside Tick() (Host, JoinLobby, JoinUdp, Leave) is queued and
// delivered at the start of the next Tick(); State() itself updates immediately.
#include "qdcoop/net/Session.h"

#include "qdcoop/core/Log.h"
#include "qdcoop/net/Protocol.h"
#include "qdcoop/net/SessionInternal.h"
#include "qdcoop/net/SteamFlat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace qd::net {
namespace {

using Clock = std::chrono::steady_clock;

#if QD_DEV
constexpr uint16_t kDefaultUdpPort = 27777;
#endif
// Lobby data keys (replace the pre-1.0 "qdcoop" / "qdcoop_ver").
constexpr char kLobbyKeyMod[] = "kd_mod";
constexpr char kLobbyKeyAuthor[] = "kd_author";
constexpr char kLobbyKeyVersion[] = "kd_ver";
constexpr auto     kHeartbeatEvery = std::chrono::milliseconds(1000);
constexpr auto     kMemberPollEvery = std::chrono::milliseconds(500);
constexpr auto     kLobbyDataTtl = std::chrono::seconds(10);
constexpr uint8_t  kHeartbeatByte = static_cast<uint8_t>(MsgType::Heartbeat);
constexpr uint32_t kMemberGoneMask = steam::k_EChatMemberStateChangeLeft | steam::k_EChatMemberStateChangeDisconnected |
                                     steam::k_EChatMemberStateChangeKicked | steam::k_EChatMemberStateChangeBanned;
// CSteamID: bits 0-31 account, 32-51 instance, 52-55 EAccountType, 56-63 universe. Lobbies are k_EAccountTypeChat
// (8; 7 is Clan), e.g. 109775243986175862 == 0x01860000'B6ED8776.
constexpr uint64_t kEAccountTypeChat = 8;

constexpr auto    kKeepaliveWake = std::chrono::milliseconds(250);   // keepalive thread cadence
constexpr int64_t kKeepaliveStallNs = 1'500'000'000;                  // Tick() older than this => game thread stalled
constexpr int64_t kKeepaliveMaxStallNs = 120'000'000'000;             // ... but give up covering for it after 2 min

int64_t ToNs(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

using IoMutex = std::recursive_mutex;

// Traffic counters (payload bytes, without the transport's own headers). Session::Tick logs them once a minute
// while somebody is connected.
std::atomic<uint64_t> g_sentBytes{0}, g_recvBytes{0}, g_sentPackets{0}, g_recvPackets{0};

// Serialises every ITransport call on the session's I/O lock. Holds references to the Session's lock and transport
// slot, so it always forwards to the current transport (UDP Host() swaps it) and is a no-op while there is none.
// Recursive lock: the Session sometimes holds `io` around a sequence of calls and still calls through here.
class LockedTransport final : public ITransport {
public:
    LockedTransport(IoMutex& m, std::unique_ptr<ITransport>& t) : m_(m), t_(t) {}

    bool Init() override {
        std::lock_guard l(m_);
        return t_ && t_->Init();
    }
    void Shutdown() override {
        std::lock_guard l(m_);
        if (t_) t_->Shutdown();
    }
    bool Send(PeerId to, Channel ch, const void* data, size_t size) override {
        std::lock_guard l(m_);
        const bool ok = t_ && t_->Send(to, ch, data, size);
        if (ok) {
            g_sentBytes.fetch_add(size, std::memory_order_relaxed);
            g_sentPackets.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }
    void Poll(std::vector<Packet>& out) override {
        std::lock_guard l(m_);
        if (!t_) return;
        const size_t before = out.size();
        t_->Poll(out);
        uint64_t bytes = 0;
        for (size_t i = before; i < out.size(); ++i) bytes += out[i].data.size();
        g_recvBytes.fetch_add(bytes, std::memory_order_relaxed);
        g_recvPackets.fetch_add(out.size() - before, std::memory_order_relaxed);
    }
    void OpenPeer(PeerId peer) override {
        std::lock_guard l(m_);
        if (t_) t_->OpenPeer(peer);
    }
    void ClosePeer(PeerId peer) override {
        std::lock_guard l(m_);
        if (t_) t_->ClosePeer(peer);
    }
    PeerId LocalId() const override {
        std::lock_guard l(m_);
        return t_ ? t_->LocalId() : kInvalidPeer;
    }
    int PingMs(PeerId peer) const override {
        std::lock_guard l(m_);
        return t_ ? t_->PingMs(peer) : -1;
    }

private:
    IoMutex&                     m_;
    std::unique_ptr<ITransport>& t_;
};

const char* StateName(SessionState s) {
    switch (s) {
        case SessionState::Idle: return "Idle";
        case SessionState::CreatingLobby: return "CreatingLobby";
        case SessionState::Hosting: return "Hosting";
        case SessionState::JoiningLobby: return "JoiningLobby";
        case SessionState::Connecting: return "Connecting";
        case SessionState::Connected: return "Connected";
        case SessionState::Failed: return "Failed";
    }
    return "?";
}

bool LooksLikeLobbyId(uint64_t id) { return id != 0 && ((id >> 52) & 0xF) == kEAccountTypeChat; }

// Finds "+connect_lobby <digits>" in a command line or rich-presence connect string. 0 if absent.
template <class C>
uint64_t ParseConnectLobby(const C* s) {
    if (!s) return 0;
    static constexpr char kKey[] = "+connect_lobby";
    for (const C* p = s; *p; ++p) {
        size_t i = 0;
        while (kKey[i] && p[i] == static_cast<C>(kKey[i])) ++i;
        if (kKey[i] != 0) continue;
        const C* q = p + i;
        while (*q == C(' ') || *q == C('\t') || *q == C('"') || *q == C('=')) ++q;
        uint64_t v = 0;
        int      digits = 0;
        while (*q >= C('0') && *q <= C('9') && digits < 20) {
            v = v * 10 + static_cast<uint64_t>(*q - C('0'));
            ++q;
            ++digits;
        }
        return digits ? v : 0;
    }
    return 0;
}

std::string SafeStr(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

struct Session::Impl {
    explicit Impl(Session& s) : self(s) {}

    Session& self;

    SessionConfig               cfg;
    bool                        inited = false;
    bool                        inTick = false;

    // ---- transport + I/O lock. `transport` is only (re)assigned while holding `io`; all calls go through `tx`.
    mutable IoMutex             io;
    std::unique_ptr<ITransport> transport;
    LockedTransport             tx{io, transport};

    // ---- keepalive thread
    std::vector<PeerId>   keepalivePeers;          // guarded by `io`: copy of `connected`, refreshed every Tick()
    std::atomic<int64_t>  lastTickNs{0};           // steady_clock ns of the last Tick() start
    std::atomic<bool>     keepaliveEnabled{true};  // test hook
    std::atomic<uint32_t> keepaliveSent{0};        // heartbeats sent by the thread since Init (test hook)
    std::atomic<uint32_t> keepaliveSinceTick{0};   // ... since the last Tick() (for the "was stalled" log line)
    std::thread           keepalive;
    std::mutex            kaMutex;                 // guards kaStop; pairs with kaCv
    std::condition_variable kaCv;
    bool                  kaStop = false;

    SessionState              state = SessionState::Idle;
    std::vector<SessionState> deferredStates;
    bool                      isHost = false;
    std::string               lastError;
    PeerId                    hostId = kInvalidPeer;
    uint64_t                  lobbyId = 0;
    uint64_t                  pendingInvite = 0;
    uint64_t                  cmdLineLobby = 0;
    uint32_t                  generation = 0;  // bumped by Host/Join/Leave; stale work checks it and bails out

    struct PeerInfo {
        Clock::time_point lastHeard;
    };
    std::map<PeerId, PeerInfo> connected;  // remote peers we have heard from
    std::set<PeerId>           pending;    // opened, waiting for their first packet
    Clock::time_point          connectStart{};
    Clock::time_point          lastHeartbeat{};
    Clock::time_point          lastMemberPoll{};
    bool                       forceMemberPoll = false;
    bool                       loggedFull = false;

    // ---- Steam only
    const steam::Api* api = nullptr;
    std::unique_ptr<steam::CallbackQueue<steam::GameLobbyJoinRequested_t>>        cbLobbyJoin;
    std::unique_ptr<steam::CallbackQueue<steam::GameRichPresenceJoinRequested_t>> cbRichJoin;
    std::unique_ptr<steam::CallbackQueue<steam::LobbyChatUpdate_t>>               cbChatUpdate;
    bool richPresenceSet = false;

    enum class CallKind : uint8_t { CreateLobby, JoinLobby };
    struct ApiCall {
        steam::SteamAPICall_t handle = steam::k_uAPICallInvalid;
        CallKind              kind = CallKind::CreateLobby;
        uint32_t              generation = 0;  // != current generation => abandoned (leave the lobby when it lands)
    };
    std::vector<ApiCall> calls;

    struct LobbyRequest {
        uint64_t          lobby = 0;
        uint64_t          friendId = 0;
        Clock::time_point since{};
    };
    std::vector<LobbyRequest> lobbyRequests;
    std::vector<LobbyInfo>    friendLobbies;

    // ------------------------------------------------------------ helpers

    bool Steam() const { return cfg.kind == TransportKind::Steam && api != nullptr; }
    PeerId Local() const { return tx.LocalId(); }   // kInvalidPeer without a transport

    // ------------------------------------------------------------ keepalive

    void StartKeepalive() {
        {
            std::lock_guard l(kaMutex);
            kaStop = false;
        }
        keepaliveSent = 0;
        keepaliveSinceTick = 0;
        lastTickNs = ToNs(Clock::now());
        keepalive = std::thread([this] { KeepaliveLoop(); });
    }

    // Returns false when the thread was already gone before we asked it to stop: that only happens during process
    // teardown (ExitProcess kills every other thread before our static destructors run), possibly while it held `io`
    // or kaMutex. The caller must then not touch either lock again.
    bool StopKeepalive() {
        if (!keepalive.joinable()) return true;
        if (WaitForSingleObject(keepalive.native_handle(), 0) == WAIT_OBJECT_0) {
            keepalive.detach();
            return false;
        }
        {
            std::lock_guard l(kaMutex);
            kaStop = true;
        }
        kaCv.notify_all();
        keepalive.join();
        return true;
    }

    void KeepaliveLoop() {
        std::unique_lock lk(kaMutex);
        while (!kaStop) {
            kaCv.wait_for(lk, kKeepaliveWake, [this] { return kaStop; });
            if (kaStop) break;
            lk.unlock();
            KeepaliveOnce();
            lk.lock();
        }
    }

    // Keepalive thread. Heartbeats only: no receiving, no timeout decisions, no callbacks.
    void KeepaliveOnce() {
        if (!keepaliveEnabled.load(std::memory_order_relaxed)) return;
        const int64_t stalledNs = ToNs(Clock::now()) - lastTickNs.load(std::memory_order_relaxed);
        if (stalledNs < kKeepaliveStallNs) return;
        if (stalledNs > kKeepaliveMaxStallNs) return;   // hung, not loading: let the peers drop us
        // Quitting the game also stops Tick(), and the engine shuts Steam down afterwards; never call into a Steam that
        // is gone. (After SteamAPI_Shutdown the interface accessor returns null. `api`/`cfg` only change while this
        // thread is stopped.)
        if (cfg.kind == TransportKind::Steam &&
            (!api || !steam::ModuleStillLoaded() || !api->SteamAPI_SteamNetworkingMessages_SteamAPI_v002()))
            return;
        std::lock_guard l(io);
        if (!transport) return;
        for (PeerId p : keepalivePeers) {
            if (transport->Send(p, Channel::Unreliable, &kHeartbeatByte, 1)) {
                keepaliveSent.fetch_add(1, std::memory_order_relaxed);
                keepaliveSinceTick.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Game thread, end of Tick(): hand the keepalive thread the current peer list.
    void SyncKeepalivePeers() {
        std::lock_guard l(io);
        keepalivePeers.clear();
        for (const auto& [p, info] : connected) keepalivePeers.push_back(p);
    }

    void SetState(SessionState s) {
        if (state == s) return;
        QD_INFO("net", "session: {} -> {}", StateName(state), StateName(s));
        state = s;
        if (inTick) {
            if (self.OnStateChanged) self.OnStateChanged(s);
        } else {
            deferredStates.push_back(s);
        }
    }

    void FlushDeferredStates() {
        if (deferredStates.empty()) return;
        std::vector<SessionState> list = std::move(deferredStates);
        deferredStates.clear();
        const uint32_t gen = generation;
        for (SessionState s : list) {
            if (self.OnStateChanged) self.OnStateChanged(s);
            if (gen != generation || !inited) break;  // the callback started something new; older states are moot
        }
    }

    void Fail(std::string why) {
        QD_WARN("net", "session: failed: {}", why);
        lastError = std::move(why);
        SetState(SessionState::Failed);
    }

    void BeginAttempt(bool asHost) {
        ++generation;
        lastError.clear();
        ResetPeers();
        isHost = asHost;
        hostId = kInvalidPeer;
        lobbyId = 0;
        loggedFull = false;
    }

    void ResetPeers() {
        std::lock_guard l(io);   // one critical section: the keepalive thread must not see half-closed peers
        if (transport) {
            for (const auto& [p, info] : connected) transport->ClosePeer(p);
            for (PeerId p : pending) transport->ClosePeer(p);
        }
        connected.clear();
        pending.clear();
        keepalivePeers.clear();
    }

    void SendHeartbeat(PeerId p) {
        if (p != kInvalidPeer) tx.Send(p, Channel::Unreliable, &kHeartbeatByte, 1);
    }

    void StartConnecting(Clock::time_point now) {
        tx.OpenPeer(hostId);
        pending.insert(hostId);
        connectStart = now;
        SendHeartbeat(hostId);
        SetState(SessionState::Connecting);
    }

    void DisconnectPeer(PeerId p, const char* why) {
        connected.erase(p);
        pending.erase(p);
        tx.ClosePeer(p);
        QD_INFO("net", "session: peer {:016x} disconnected ({})", p, why);
        if (self.OnPeerDisconnected) self.OnPeerDisconnected(p);
    }

    // Client lost the host: OnPeerDisconnected(host) (if it had connected), ClosePeer, then Failed.
    void HostLost(const char* why) {
        const PeerId   h = hostId;
        const uint32_t gen = generation;
        const bool     wasConnected = connected.contains(h);
        if (wasConnected) {
            DisconnectPeer(h, why);
            if (gen != generation || !inited) return;
        } else {
            pending.erase(h);
            tx.ClosePeer(h);
        }
        Fail(why);
    }

    // ------------------------------------------------------------ packets

    // Returns true if packets from `from` should be processed (possibly connecting it right now).
    bool AdmitPacket(PeerId from, Clock::time_point now) {
        if (auto it = connected.find(from); it != connected.end()) {
            it->second.lastHeard = now;
            return true;
        }
        const uint32_t gen = generation;
        if (isHost) {
            if (state != SessionState::Hosting) return false;
            // Steam: only lobby members (opened by the member poll). UDP: anyone who knows our port.
            if (cfg.kind == TransportKind::Steam && !pending.contains(from)) return false;
            if (static_cast<int>(connected.size()) + 1 >= cfg.maxMembers) {
                if (!loggedFull) {
                    QD_WARN("net", "session: room is full ({}), ignoring {:016x}", cfg.maxMembers, from);
                    loggedFull = true;
                }
                return false;
            }
            pending.erase(from);
            connected[from] = PeerInfo{now};
            QD_INFO("net", "session: peer {:016x} connected", from);
            SendHeartbeat(from);  // lets the client leave Connecting without waiting for the 1 s heartbeat
            if (self.OnPeerConnected) self.OnPeerConnected(from);
            return gen == generation && inited;
        }
        if (state != SessionState::Connecting || from != hostId) return false;
        pending.erase(from);
        connected[from] = PeerInfo{now};
        QD_INFO("net", "session: connected to host {:016x}", from);
        SetState(SessionState::Connected);
        if (gen != generation || !inited) return false;
        if (self.OnPeerConnected) self.OnPeerConnected(from);
        return gen == generation && inited;
    }

    void PumpPackets(Clock::time_point now) {
        std::vector<Packet> packets;
        tx.Poll(packets);   // the lock is released before any callback runs (callbacks Send through `tx` again)
        const uint32_t gen = generation;
        for (const Packet& p : packets) {
            if (gen != generation || !inited) return;
            if (!AdmitPacket(p.from, now)) continue;
            if (PeekType(p.data.data(), p.data.size()) == MsgType::Heartbeat) continue;
            if (self.OnPacket) self.OnPacket(p);
        }
    }

    void SendHeartbeats(Clock::time_point now) {
        if (now - lastHeartbeat < kHeartbeatEvery) return;
        lastHeartbeat = now;
        for (const auto& [p, info] : connected) SendHeartbeat(p);
        // Not-yet-connected peers: the client keeps knocking on the host; a Steam host keeps the new members' sessions warm.
        if (!isHost && state == SessionState::Connecting) SendHeartbeat(hostId);
        if (isHost && cfg.kind == TransportKind::Steam)
            for (PeerId p : pending) SendHeartbeat(p);
    }

    void CheckTimeouts(Clock::time_point now) {
        const auto timeout = std::chrono::milliseconds(cfg.timeoutMs);
        if (!isHost && state == SessionState::Connecting && now - connectStart > timeout) {
            pending.erase(hostId);
            tx.ClosePeer(hostId);
            Fail("host did not respond");
            return;
        }
        std::vector<PeerId> gone;
        for (const auto& [p, info] : connected)
            if (now - info.lastHeard > timeout) gone.push_back(p);
        const uint32_t gen = generation;
        for (PeerId p : gone) {
            if (gen != generation || !inited) return;
            if (!isHost && p == hostId)
                HostLost("host lost");
            else
                DisconnectPeer(p, "timed out");
        }
    }

    // ------------------------------------------------------------ Steam

    void HandleInvite(uint64_t lobby, const char* source) {
        if (!LooksLikeLobbyId(lobby)) {
            QD_WARN("net", "session: {} gave an invalid lobby id {}", source, lobby);
            return;
        }
        if (lobby == lobbyId) return;  // already there
        QD_INFO("net", "session: {} -> lobby {}", source, lobby);
        if (state == SessionState::Idle)
            self.JoinLobby(lobby);
        else
            pendingInvite = lobby;
    }

    void TickSteam(Clock::time_point now) {
        if (cbLobbyJoin)
            for (const auto& ev : cbLobbyJoin->Drain()) {
                HandleInvite(ev.m_steamIDLobby, "lobby join request");
                if (!inited) return;
            }
        if (cbRichJoin)
            for (auto& ev : cbRichJoin->Drain()) {
                ev.m_rgchConnect[sizeof(ev.m_rgchConnect) - 1] = 0;
                if (const uint64_t id = ParseConnectLobby(ev.m_rgchConnect); id != 0)
                    HandleInvite(id, "rich presence join");
                else
                    QD_INFO("net", "session: rich presence join without +connect_lobby: '{}'",
                            static_cast<const char*>(ev.m_rgchConnect));
                if (!inited) return;
            }
        if (cmdLineLobby) {
            const uint64_t id = std::exchange(cmdLineLobby, 0);
            HandleInvite(id, "command line +connect_lobby");
            if (!inited) return;
        }

        if (cbChatUpdate)
            for (const auto& ev : cbChatUpdate->Drain()) {
                if (lobbyId == 0 || ev.m_ulSteamIDLobby != lobbyId) continue;
                if (isHost) {
                    forceMemberPoll = true;
                } else if (ev.m_ulSteamIDUserChanged == hostId && (ev.m_rgfChatMemberStateChange & kMemberGoneMask) &&
                           (state == SessionState::Connecting || state == SessionState::Connected)) {
                    HostLost("host left the lobby");
                    if (!inited) return;
                }
            }

        PollApiCalls(now);
        if (!inited) return;

        if (isHost && state == SessionState::Hosting && lobbyId != 0 &&
            (forceMemberPoll || now - lastMemberPoll >= kMemberPollEvery))
            PollLobbyMembers(now);
        if (!inited) return;

        PollFriendLobbyData(now);
    }

    void PollApiCalls(Clock::time_point now) {
        for (size_t i = 0; i < calls.size();) {
            const ApiCall c = calls[i];
            bool          failed = false;
            if (!api->SteamAPI_ISteamUtils_IsAPICallCompleted(api->utils, c.handle, &failed)) {
                ++i;
                continue;
            }
            calls.erase(calls.begin() + static_cast<std::ptrdiff_t>(i));
            if (c.kind == CallKind::CreateLobby)
                OnLobbyCreated(c, failed);
            else
                OnLobbyEntered(c, failed, now);
            if (!inited) return;
        }
    }

    void OnLobbyCreated(const ApiCall& c, bool failed) {
        steam::LobbyCreated_t r{};
        bool                  ioFailed = false;
        const bool ok = !failed &&
                        api->SteamAPI_ISteamUtils_GetAPICallResult(api->utils, c.handle, &r, static_cast<int>(sizeof(r)),
                                                                    steam::LobbyCreated_t::k_iCallback, &ioFailed) &&
                        !ioFailed && r.m_eResult == steam::k_EResultOK && r.m_ulSteamIDLobby != 0;
        if (c.generation != generation) {
            if (ok) {
                api->SteamAPI_ISteamMatchmaking_LeaveLobby(api->matchmaking, r.m_ulSteamIDLobby);
                QD_INFO("net", "session: left lobby {} created by an abandoned Host()", r.m_ulSteamIDLobby);
            }
            return;
        }
        if (state != SessionState::CreatingLobby) return;
        if (!ok) {
            Fail(std::format("CreateLobby failed (EResult {}, ioFailure {})", r.m_eResult, failed || ioFailed));
            return;
        }

        lobbyId = r.m_ulSteamIDLobby;
        hostId = Local();
        std::string name = cfg.displayName;
        if (name.empty()) name = SafeStr(api->SteamAPI_ISteamFriends_GetPersonaName(api->friends));
        auto* mm = api->matchmaking;
        api->SteamAPI_ISteamMatchmaking_SetLobbyData(mm, lobbyId, kLobbyKeyVersion, cfg.modVersion.c_str());
        api->SteamAPI_ISteamMatchmaking_SetLobbyData(mm, lobbyId, "game_build", cfg.gameBuild.c_str());
        api->SteamAPI_ISteamMatchmaking_SetLobbyData(mm, lobbyId, "host_name", name.c_str());
        api->SteamAPI_ISteamMatchmaking_SetLobbyData(mm, lobbyId, kLobbyKeyAuthor, cfg.modAuthor.c_str());
        api->SteamAPI_ISteamMatchmaking_SetLobbyData(mm, lobbyId, kLobbyKeyMod, cfg.modName.c_str());
        const std::string connect = std::format("+connect_lobby {}", lobbyId);
        const std::string status = cfg.modName.empty() ? std::string("co-op") : cfg.modName + " co-op";
        api->SteamAPI_ISteamFriends_SetRichPresence(api->friends, "connect", connect.c_str());
        api->SteamAPI_ISteamFriends_SetRichPresence(api->friends, "status", status.c_str());
        richPresenceSet = true;
        forceMemberPoll = true;
        QD_INFO("net", "session: hosting Steam lobby {}", lobbyId);
        SetState(SessionState::Hosting);
    }

    void OnLobbyEntered(const ApiCall& c, bool failed, Clock::time_point now) {
        steam::LobbyEnter_t r{};
        bool                ioFailed = false;
        const bool          gotResult =
            !failed && api->SteamAPI_ISteamUtils_GetAPICallResult(api->utils, c.handle, &r, static_cast<int>(sizeof(r)),
                                                                  steam::LobbyEnter_t::k_iCallback, &ioFailed) &&
            !ioFailed;
        const bool ok = gotResult && r.m_EChatRoomEnterResponse == steam::k_EChatRoomEnterResponseSuccess &&
                        r.m_ulSteamIDLobby != 0;
        if (c.generation != generation) {
            if (ok) {
                api->SteamAPI_ISteamMatchmaking_LeaveLobby(api->matchmaking, r.m_ulSteamIDLobby);
                QD_INFO("net", "session: left lobby {} entered by an abandoned JoinLobby()", r.m_ulSteamIDLobby);
            }
            return;
        }
        if (state != SessionState::JoiningLobby) return;
        if (!ok) {
            Fail(std::format("JoinLobby failed (response {}, ioFailure {})", r.m_EChatRoomEnterResponse,
                             failed || ioFailed || !gotResult));
            return;
        }

        lobbyId = r.m_ulSteamIDLobby;
        const std::string tag = SafeStr(api->SteamAPI_ISteamMatchmaking_GetLobbyData(api->matchmaking, lobbyId, kLobbyKeyMod));
        if (tag != cfg.modName)
            QD_WARN("net", "session: lobby {} has {}='{}' (expected '{}'); trying anyway (the host checks the Hello)",
                    lobbyId, kLobbyKeyMod, tag, cfg.modName);
        const PeerId owner = api->SteamAPI_ISteamMatchmaking_GetLobbyOwner(api->matchmaking, lobbyId);
        if (owner == kInvalidPeer || owner == Local()) {
            Fail("lobby has no other owner to connect to");
            return;
        }
        hostId = owner;
        QD_INFO("net", "session: entered lobby {}, host {}", lobbyId, owner);
        StartConnecting(now);
    }

    void PollLobbyMembers(Clock::time_point now) {
        lastMemberPoll = now;
        forceMemberPoll = false;
        auto*     mm = api->matchmaking;
        const int n = api->SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(mm, lobbyId);
        if (n <= 0) return;  // data not available right now; never treat that as "everyone left"

        const PeerId     self_id = Local();
        std::set<PeerId> members;
        for (int i = 0; i < n; ++i) {
            const PeerId id = api->SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(mm, lobbyId, i);
            if (id != kInvalidPeer && id != self_id) members.insert(id);
        }
        for (PeerId m : members) {
            if (connected.contains(m) || pending.contains(m)) continue;
            QD_INFO("net", "session: new lobby member {}", m);
            pending.insert(m);
            tx.OpenPeer(m);
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (!members.contains(*it)) {
                QD_INFO("net", "session: lobby member {} left before connecting", *it);
                tx.ClosePeer(*it);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<PeerId> gone;
        for (const auto& [p, info] : connected)
            if (!members.contains(p)) gone.push_back(p);
        const uint32_t gen = generation;
        for (PeerId p : gone) {
            if (gen != generation || !inited) return;
            DisconnectPeer(p, "left the lobby");
        }
    }

    void PollFriendLobbyData(Clock::time_point now) {
        auto* mm = api->matchmaking;
        for (auto it = lobbyRequests.begin(); it != lobbyRequests.end();) {
            const std::string tag = SafeStr(api->SteamAPI_ISteamMatchmaking_GetLobbyData(mm, it->lobby, kLobbyKeyMod));
            if (!cfg.modName.empty() && tag == cfg.modName) {
                LobbyInfo info;
                info.lobbyId = it->lobby;
                info.ownerId = api->SteamAPI_ISteamMatchmaking_GetLobbyOwner(mm, it->lobby);
                if (info.ownerId == kInvalidPeer) info.ownerId = it->friendId;
                info.hostName = SafeStr(api->SteamAPI_ISteamMatchmaking_GetLobbyData(mm, it->lobby, "host_name"));
                if (info.hostName.empty())
                    info.hostName = SafeStr(api->SteamAPI_ISteamFriends_GetFriendPersonaName(api->friends, it->friendId));
                info.modName = tag;
                info.modAuthor = SafeStr(api->SteamAPI_ISteamMatchmaking_GetLobbyData(mm, it->lobby, kLobbyKeyAuthor));
                info.modVersion = SafeStr(api->SteamAPI_ISteamMatchmaking_GetLobbyData(mm, it->lobby, kLobbyKeyVersion));
                info.gameBuild = SafeStr(api->SteamAPI_ISteamMatchmaking_GetLobbyData(mm, it->lobby, "game_build"));
                info.members = api->SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(mm, it->lobby);
                info.maxMembers = api->SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit
                                      ? api->SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(mm, it->lobby)
                                      : 0;
                auto existing = std::find_if(friendLobbies.begin(), friendLobbies.end(),
                                             [&](const LobbyInfo& l) { return l.lobbyId == info.lobbyId; });
                if (existing != friendLobbies.end())
                    *existing = std::move(info);
                else
                    friendLobbies.push_back(std::move(info));
                it = lobbyRequests.erase(it);
            } else if (now - it->since > kLobbyDataTtl) {
                it = lobbyRequests.erase(it);  // not a lobby of this mod, or the data never came
            } else {
                ++it;
            }
        }
    }
};

// ================================================================ Session

Session::Session() : impl_(new Impl(*this)) {}

Session::~Session() {
    Shutdown();
    delete impl_;
}

bool Session::Init(const SessionConfig& cfg) {
    Impl& d = *impl_;
    if (d.inited) Shutdown();

    d.cfg = cfg;
    if (d.cfg.maxMembers < 2 || d.cfg.maxMembers > kMaxPlayers) {
        QD_WARN("net", "session: maxMembers {} out of range, clamping to 2..{}", d.cfg.maxMembers, kMaxPlayers);
        d.cfg.maxMembers = std::clamp(d.cfg.maxMembers, 2, kMaxPlayers);
    }
    if (d.cfg.timeoutMs < 100) d.cfg.timeoutMs = 100;
    d.lastError.clear();
    d.state = SessionState::Idle;
    d.deferredStates.clear();

    if (d.cfg.kind == TransportKind::Steam) {
        if (!steam::Load()) {
            d.lastError = "Steam API unavailable";
            return false;
        }
        d.api = steam::Get();
        d.transport = CreateSteamTransport();
        if (!d.transport->Init()) {
            d.transport.reset();
            d.api = nullptr;
            d.lastError = "Steam transport init failed";
            return false;
        }
        d.cbLobbyJoin = std::make_unique<steam::CallbackQueue<steam::GameLobbyJoinRequested_t>>();
        d.cbRichJoin = std::make_unique<steam::CallbackQueue<steam::GameRichPresenceJoinRequested_t>>();
        d.cbChatUpdate = std::make_unique<steam::CallbackQueue<steam::LobbyChatUpdate_t>>();
        if (!d.cbLobbyJoin->Register() || !d.cbRichJoin->Register() || !d.cbChatUpdate->Register())
            QD_WARN("net", "session: could not register every Steam callback");
        d.cmdLineLobby = ParseConnectLobby(GetCommandLineW());
        if (d.cmdLineLobby) QD_INFO("net", "session: command line asks to join lobby {}", d.cmdLineLobby);
    } else {
#if QD_DEV
        d.api = nullptr;
        d.transport = CreateUdpTransport(0);
        if (!d.transport->Init()) {
            d.transport.reset();
            d.lastError = "UDP transport init failed";
            return false;
        }
#else
        d.api = nullptr;
        d.lastError = "UDP transport is not available in this build";
        QD_WARN("net", "session: {}", d.lastError);
        return false;
#endif
    }
    d.inited = true;
    d.StartKeepalive();
    QD_INFO("net", "session: initialised ({} transport, local id {:016x}, timeout {} ms)",
            d.cfg.kind == TransportKind::Steam ? "Steam" : "UDP", d.Local(), d.cfg.timeoutMs);
    return true;
}

void Session::Shutdown() {
    Impl& d = *impl_;
    if (!d.inited) return;
    if (!d.StopKeepalive()) {
        // Process teardown (see StopKeepalive): the OS may have killed the keepalive thread while it held `io`, and the
        // Steam callback thread is gone too. Touch nothing — no locks, no Steam calls — and let the process exit.
        d.inited = false;
        (void)d.transport.release();
        (void)d.cbLobbyJoin.release();
        (void)d.cbRichJoin.release();
        (void)d.cbChatUpdate.release();
        d.api = nullptr;
        return;
    }
    Leave();
    if (!d.calls.empty())
        QD_WARN("net", "session: shutting down with {} Steam API call(s) still in flight", d.calls.size());
    d.calls.clear();
    d.cbLobbyJoin.reset();
    d.cbRichJoin.reset();
    d.cbChatUpdate.reset();
    {
        std::lock_guard l(d.io);   // the keepalive thread is already stopped; this only fences Transport() users
        if (d.transport) d.transport->Shutdown();
        d.transport.reset();
        d.keepalivePeers.clear();
    }
    d.api = nullptr;
    d.lobbyRequests.clear();
    d.friendLobbies.clear();
    d.deferredStates.clear();
    d.cmdLineLobby = 0;
    d.inited = false;
    QD_INFO("net", "session: shut down");
}

void Session::Tick(float /*dt*/) {
    Impl& d = *impl_;
    if (!d.inited) return;
    {   // one traffic line a minute while connected
        static Clock::time_point last = Clock::now();
        static uint64_t ls = 0, lr = 0, lsp = 0, lrp = 0;
        const auto now = Clock::now();
        const double sec = std::chrono::duration<double>(now - last).count();
        if (sec >= 60.0) {
            const uint64_t sb = g_sentBytes.load(), rb = g_recvBytes.load(), sp = g_sentPackets.load(), rp = g_recvPackets.load();
            if (!d.connected.empty()) {
                int ping = -1;
                for (const auto& [p, info] : d.connected) ping = std::max(ping, d.tx.PingMs(p));
                QD_INFO("net", "traffic {:.0f} s: sent {:.1f} KB ({:.0f} B/s, {:.0f} pkt/s), received {:.1f} KB ({:.0f} B/s, {:.0f} pkt/s), "
                               "{} peer(s), worst ping {} ms",
                        sec, (sb - ls) / 1024.0, (sb - ls) / sec, (sp - lsp) / sec, (rb - lr) / 1024.0, (rb - lr) / sec,
                        (rp - lrp) / sec, d.connected.size(), ping);
            }
            last = now;
            ls = sb; lr = rb; lsp = sp; lrp = rp;
        }
    }
    d.inTick = true;
    struct TickGuard {
        bool& flag;
        ~TickGuard() { flag = false; }
    } guard{d.inTick};

    // Tell the keepalive thread the game thread is alive (and report what it did if we had been stalled).
    {
        const int64_t nowNs = ToNs(Clock::now());
        const int64_t prevNs = d.lastTickNs.exchange(nowNs, std::memory_order_relaxed);
        if (const uint32_t sent = d.keepaliveSinceTick.exchange(0, std::memory_order_relaxed); sent > 0)
            QD_INFO("net", "session: game thread was stalled for {:.1f} s; keepalive sent {} heartbeat(s) meanwhile",
                    static_cast<double>(nowNs - prevNs) / 1e9, sent);
    }

    d.FlushDeferredStates();
    if (!d.inited) return;

    const auto now = Clock::now();  // wall clock, not dt: liveness must not stretch with time dilation
    if (d.Steam()) {
        d.TickSteam(now);
        if (!d.inited) return;
    }
    if (d.transport) {
        d.PumpPackets(now);   // receive BEFORE judging timeouts: after a stall, the backlog proves the peers are alive
        if (!d.inited) return;
    }
    d.SendHeartbeats(now);
    d.CheckTimeouts(now);
    if (!d.inited) return;
    d.SyncKeepalivePeers();
}

void Session::Host() {
    Impl& d = *impl_;
    if (!d.inited) {
        QD_WARN("net", "session: Host() before Init()");
        return;
    }
    if (d.state != SessionState::Idle) {
        QD_WARN("net", "session: Host() ignored in state {}", StateName(d.state));
        return;
    }
    d.BeginAttempt(true);

    if (d.Steam()) {
        const steam::SteamAPICall_t h = d.api->SteamAPI_ISteamMatchmaking_CreateLobby(
            d.api->matchmaking, steam::k_ELobbyTypeFriendsOnly, d.cfg.maxMembers);
        if (h == steam::k_uAPICallInvalid) {
            d.Fail("CreateLobby could not be started");
            return;
        }
        d.calls.push_back({h, Impl::CallKind::CreateLobby, d.generation});
        d.SetState(SessionState::CreatingLobby);
        return;
    }

#if QD_DEV
    const uint16_t port = d.cfg.udpListenPort ? d.cfg.udpListenPort : kDefaultUdpPort;
    const PeerId   want = MakeUdpPeerId("127.0.0.1", port);
    bool           bindFailed = false;
    {
        std::lock_guard l(d.io);   // swapping the transport: the keepalive thread must not be inside it
        if (!d.transport || d.transport->LocalId() != want) {
            if (d.transport) d.transport->Shutdown();  // free our current port before binding the listen port
            d.transport = CreateUdpTransport(port);
            if (!d.transport->Init()) {
                d.transport = CreateUdpTransport(0);  // keep a usable transport around
                d.transport->Init();
                bindFailed = true;
            }
        }
    }
    if (bindFailed) {
        d.Fail(std::format("cannot listen on UDP port {}", port));
        return;
    }
    d.hostId = d.Local();
    QD_INFO("net", "session: hosting on UDP port {}", port);
    d.SetState(SessionState::Hosting);
#else
    d.Fail("Steam session required");   // unreachable: a release Session only initialises with Steam
#endif
}

void Session::JoinLobby(uint64_t lobbyId) {
    Impl& d = *impl_;
    if (!d.inited || !d.Steam()) {
        QD_WARN("net", "session: JoinLobby() needs an initialised Steam session");
        return;
    }
    if (d.state != SessionState::Idle) {
        QD_WARN("net", "session: JoinLobby({}) ignored in state {}", lobbyId, StateName(d.state));
        return;
    }
    if (lobbyId == 0) return;
    if (d.pendingInvite == lobbyId) d.pendingInvite = 0;
    d.BeginAttempt(false);

    const steam::SteamAPICall_t h = d.api->SteamAPI_ISteamMatchmaking_JoinLobby(d.api->matchmaking, lobbyId);
    if (h == steam::k_uAPICallInvalid) {
        d.Fail("JoinLobby could not be started");
        return;
    }
    d.calls.push_back({h, Impl::CallKind::JoinLobby, d.generation});
    QD_INFO("net", "session: joining lobby {}", lobbyId);
    d.SetState(SessionState::JoiningLobby);
}

void Session::JoinUdp(const std::string& ipv4, uint16_t port) {
#if !QD_DEV
    (void)ipv4;
    (void)port;
    QD_WARN("net", "session: JoinUdp() is not available in this build");
#else
    Impl& d = *impl_;
    if (!d.inited || d.cfg.kind != TransportKind::Udp || !d.transport) {
        QD_WARN("net", "session: JoinUdp() needs an initialised UDP session");
        return;
    }
    if (d.state != SessionState::Idle) {
        QD_WARN("net", "session: JoinUdp() ignored in state {}", StateName(d.state));
        return;
    }
    d.BeginAttempt(false);

    const PeerId host = MakeUdpPeerId(ipv4, port);
    if (host == kInvalidPeer) {
        d.Fail(std::format("bad UDP address {}:{}", ipv4, port));
        return;
    }
    if (host == d.Local()) {
        d.Fail("cannot join our own UDP port");
        return;
    }
    d.hostId = host;
    QD_INFO("net", "session: joining UDP host {}:{}", ipv4, port);
    d.StartConnecting(Clock::now());
#endif
}

void Session::Leave() {
    Impl& d = *impl_;
    if (!d.inited) return;
    ++d.generation;
    if (d.Steam()) {
        if (d.lobbyId != 0) {
            d.api->SteamAPI_ISteamMatchmaking_LeaveLobby(d.api->matchmaking, d.lobbyId);
            QD_INFO("net", "session: left lobby {}", d.lobbyId);
        }
        if (d.richPresenceSet) {
            d.api->SteamAPI_ISteamFriends_ClearRichPresence(d.api->friends);
            d.richPresenceSet = false;
        }
    }
    d.ResetPeers();
    d.lobbyId = 0;
    d.hostId = kInvalidPeer;
    d.isHost = false;
    d.SetState(SessionState::Idle);
}

void Session::RefreshFriendLobbies() {
    Impl& d = *impl_;
    if (!d.inited || !d.Steam()) return;
    d.friendLobbies.clear();
    d.lobbyRequests.clear();
    const auto  now = Clock::now();
    const auto* api = d.api;
    const int   n = api->SteamAPI_ISteamFriends_GetFriendCount(api->friends, steam::k_EFriendFlagImmediate);
    for (int i = 0; i < n; ++i) {
        const uint64_t friendId = api->SteamAPI_ISteamFriends_GetFriendByIndex(api->friends, i, steam::k_EFriendFlagImmediate);
        if (friendId == 0) continue;
        steam::FriendGameInfo_t info{};
        if (!api->SteamAPI_ISteamFriends_GetFriendGamePlayed(api->friends, friendId, &info)) continue;
        if (!LooksLikeLobbyId(info.m_steamIDLobby)) continue;
        const bool dup = std::any_of(d.lobbyRequests.begin(), d.lobbyRequests.end(),
                                     [&](const Impl::LobbyRequest& r) { return r.lobby == info.m_steamIDLobby; });
        if (dup) continue;
        if (!api->SteamAPI_ISteamMatchmaking_RequestLobbyData(api->matchmaking, info.m_steamIDLobby)) continue;
        d.lobbyRequests.push_back({info.m_steamIDLobby, friendId, now});
    }
    QD_INFO("net", "session: requested lobby data for {} friend lobbies", d.lobbyRequests.size());
}

const std::vector<LobbyInfo>& Session::FriendLobbies() const { return impl_->friendLobbies; }

uint64_t Session::PendingInviteLobby() const { return impl_->pendingInvite; }

bool Session::IsHost() const { return impl_->isHost; }

SessionState Session::State() const { return impl_->state; }

const std::string& Session::LastError() const { return impl_->lastError; }

PeerId Session::LocalId() const { return impl_->Local(); }

PeerId Session::HostId() const { return impl_->isHost ? impl_->Local() : impl_->hostId; }

std::vector<PeerId> Session::Peers() const {
    std::vector<PeerId> out;
    out.reserve(impl_->connected.size());
    for (const auto& [p, info] : impl_->connected) out.push_back(p);
    return out;
}

uint64_t Session::LobbyId() const { return impl_->lobbyId; }

// Callers get the locking wrapper, never the raw transport (it may be used by the keepalive thread at the same time).
ITransport* Session::Transport() { return impl_->transport ? &impl_->tx : nullptr; }

bool Session::Send(PeerId to, Channel ch, const void* data, size_t size) {
    return impl_->tx.Send(to, ch, data, size);
}

void Session::Broadcast(Channel ch, const void* data, size_t size, PeerId except) {
    if (!impl_->transport) return;
    for (PeerId p : Peers())
        if (p != except) impl_->tx.Send(p, ch, data, size);
}

// ================================================================ SessionInternal.h

void SessionTestAccess::SetKeepaliveEnabled(Session& s, bool enabled) {
    s.impl_->keepaliveEnabled.store(enabled, std::memory_order_relaxed);
}

uint32_t SessionTestAccess::KeepaliveHeartbeats(const Session& s) {
    return s.impl_->keepaliveSent.load(std::memory_order_relaxed);
}

}  // namespace qd::net
