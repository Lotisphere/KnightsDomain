// QD Coop — Steam transport over ISteamNetworkingMessages. Relayed by Steam, no port forwarding.
//   channel 0 -> k_nSteamNetworkingSend_Reliable, channel 1 -> k_nSteamNetworkingSend_UnreliableNoDelay
//   (our Channel value is used as the Steam channel number on both ends).
// Session acceptance does not depend on callbacks: OpenPeer() sends a 1-byte Heartbeat, which creates the session
// from our side (and implicitly accepts a pending request from that user). SessionRequest callbacks (1251) are only
// queued; Poll() accepts a request once its sender has been OpenPeer()'d (Session opens every lobby member / the
// lobby owner), and forgets it after kPendingRequestTtl.
//
// Threading: no thread-local assumptions. Send() may run on the Session's keepalive thread while the game thread
// is blocked; the Session serialises every call to this object, and ISteamNetworkingMessages itself is thread-safe
// (its service thread acks and keeps sessions alive even while nobody calls ReceiveMessagesOnChannel).
#include "qdcoop/core/Log.h"
#include "qdcoop/net/Protocol.h"
#include "qdcoop/net/SteamFlat.h"
#include "qdcoop/net/Transport.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_set>

namespace qd::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxUnreliablePayload = 1100;
constexpr size_t kMaxReliablePayload = 64 * 1024;
constexpr int    kReceiveBatch = 64;
constexpr int    kMaxBatchesPerChannel = 16;  // up to 1024 messages per channel per Poll
constexpr auto   kPendingRequestTtl = std::chrono::seconds(30);

using SessionRequestQueue = steam::CallbackQueue<steam::SteamNetworkingMessagesSessionRequest_t>;

class SteamTransport final : public ITransport {
public:
    ~SteamTransport() override { Shutdown(); }

    bool Init() override {
        if (inited_) return true;
        if (!steam::Load()) return false;
        api_ = steam::Get();
        local_ = api_->SteamAPI_ISteamUser_GetSteamID(api_->user);
        if (local_ == 0) {
            QD_ERROR("net", "steam: GetSteamID returned 0 (not logged on?)");
            return false;
        }
        requests_ = std::make_unique<SessionRequestQueue>();
        if (!requests_->Register()) {
            QD_ERROR("net", "steam: could not register SteamNetworkingMessagesSessionRequest_t");
            requests_.reset();
            return false;
        }
        inited_ = true;
        QD_INFO("net", "steam: transport ready, local id {}", local_);
        return true;
    }

    void Shutdown() override {
        if (!inited_) return;
        inited_ = false;
        const bool alive = steam::ModuleStillLoaded();
        if (alive) {
            for (PeerId p : open_) CloseSession(p);
        }
        open_.clear();
        pending_.clear();
        requests_.reset();  // unregisters the callback
        QD_INFO("net", "steam: transport shut down");
    }

    bool Send(PeerId to, Channel ch, const void* data, size_t size) override {
        if (!inited_ || !data || size == 0 || to == kInvalidPeer) return false;
        if (!open_.contains(to)) return false;
        int flags = 0;
        switch (ch) {
            case Channel::Reliable:
                if (size > kMaxReliablePayload) return false;
                flags = steam::k_nSteamNetworkingSend_ReliableNoNagle;
                break;
            case Channel::Unreliable:
                if (size > kMaxUnreliablePayload) return false;
                flags = steam::k_nSteamNetworkingSend_UnreliableNoDelay;
                break;
            default:
                return false;
        }
        const steam::SteamNetworkingIdentity id = Identity(to);
        const int32_t r = api_->SteamAPI_ISteamNetworkingMessages_SendMessageToUser(
            api_->messages, &id, data, static_cast<uint32_t>(size), flags, static_cast<int>(ch));
        if (r != steam::k_EResultOK) {
            QD_DEBUG("net", "steam: SendMessageToUser({}, ch {}, {} bytes) -> EResult {}", to, static_cast<int>(ch),
                     size, r);
            return false;
        }
        return true;
    }

    void Poll(std::vector<Packet>& out) override {
        if (!inited_) return;
        const auto now = Clock::now();

        // 1) Session requests queued by the callback thread.
        for (auto& ev : requests_->Drain()) {
            const PeerId p = api_->SteamAPI_SteamNetworkingIdentity_GetSteamID64(&ev.m_identityRemote);
            if (p == 0) {
                QD_DEBUG("net", "steam: session request from a non-SteamID identity; ignored");
                continue;
            }
            auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& x) { return x.peer == p; });
            if (it == pending_.end()) pending_.push_back({p, now});
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (open_.contains(it->peer)) {
                Accept(it->peer);
                it = pending_.erase(it);
            } else if (now - it->since > kPendingRequestTtl) {
                QD_INFO("net", "steam: ignored session request from {} (not a lobby member we opened)", it->peer);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        // 2) Messages on both channels.
        steam::SteamNetworkingMessage_t* msgs[kReceiveBatch];
        for (int ch = 0; ch <= 1; ++ch) {
            for (int batch = 0; batch < kMaxBatchesPerChannel; ++batch) {
                const int n = api_->SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(api_->messages, ch,
                                                                                              msgs, kReceiveBatch);
                if (n <= 0) break;
                for (int i = 0; i < n; ++i) {
                    steam::SteamNetworkingMessage_t* m = msgs[i];
                    if (!m) continue;
                    const PeerId from = api_->SteamAPI_SteamNetworkingIdentity_GetSteamID64(&m->m_identityPeer);
                    if (from != 0 && m->m_pData && m->m_cbSize > 0) {
                        Packet pkt;
                        pkt.from = from;
                        pkt.channel = static_cast<Channel>(ch);
                        const auto* bytes = static_cast<const uint8_t*>(m->m_pData);
                        pkt.data.assign(bytes, bytes + m->m_cbSize);
                        // Only sessions we started or accepted (both via OpenPeer) can deliver, except stragglers
                        // still queued after ClosePeer; Session ignores packets from peers it does not track.
                        out.push_back(std::move(pkt));
                    }
                    api_->SteamAPI_SteamNetworkingMessage_t_Release(m);
                }
                if (n < kReceiveBatch) break;
            }
        }
    }

    void OpenPeer(PeerId peer) override {
        if (!inited_ || peer == kInvalidPeer || peer == local_) return;
        if (!open_.insert(peer).second) return;
        QD_INFO("net", "steam: opening session with {}", peer);
        // Pending request from this user? accept it right away.
        auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& x) { return x.peer == peer; });
        if (it != pending_.end()) {
            Accept(peer);
            pending_.erase(it);
        }
        const uint8_t hb = static_cast<uint8_t>(MsgType::Heartbeat);
        Send(peer, Channel::Reliable, &hb, 1);
    }

    void ClosePeer(PeerId peer) override {
        if (!inited_) return;
        if (open_.erase(peer)) {
            CloseSession(peer);
            QD_INFO("net", "steam: closed session with {}", peer);
        }
    }

    PeerId LocalId() const override { return inited_ ? local_ : kInvalidPeer; }

    // ISteamNetworkingMessages::GetSessionConnectionInfo would give m_nPing, but it is outside the confirmed export
    // list and needs SteamNetConnectionRealTimeStatus_t's layout; report unknown for now.
    // SteamNetConnectionRealTimeStatus_t starts with { int m_eState; int m_nPing; float qualityLocal; ... } (120
    // bytes in SDK 1.63; an oversized zeroed buffer is passed). The export is optional: unknown ping without it.
    int PingMs(PeerId peer) const override {
        if (!inited_ || !api_ || !api_->SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo) return -1;
        if (!steam::ModuleStillLoaded()) return -1;
        alignas(8) uint8_t status[512] = {};
        const steam::SteamNetworkingIdentity id = Identity(peer);
        const int state = api_->SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo(api_->messages, &id, nullptr, status);
        if (state != 3) return -1;   // k_ESteamNetworkingConnectionState_Connected
        int ping = -1;
        std::memcpy(&ping, status + 4, sizeof(ping));
        return ping >= 0 && ping < 5000 ? ping : -1;
    }

private:
    struct Pending {
        PeerId            peer;
        Clock::time_point since;
    };

    steam::SteamNetworkingIdentity Identity(PeerId peer) const {
        steam::SteamNetworkingIdentity id{};  // zeroed == Clear()
        api_->SteamAPI_SteamNetworkingIdentity_SetSteamID64(&id, peer);
        return id;
    }

    void Accept(PeerId peer) {
        const steam::SteamNetworkingIdentity id = Identity(peer);
        const bool ok = api_->SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(api_->messages, &id);
        QD_INFO("net", "steam: accept session from {} -> {}", peer, ok);
    }

    void CloseSession(PeerId peer) {
        const steam::SteamNetworkingIdentity id = Identity(peer);
        api_->SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(api_->messages, &id);
    }

    const steam::Api*                    api_ = nullptr;
    bool                                 inited_ = false;
    PeerId                               local_ = kInvalidPeer;
    std::unique_ptr<SessionRequestQueue> requests_;
    std::unordered_set<PeerId>           open_;
    std::vector<Pending>                 pending_;
};

}  // namespace

std::unique_ptr<ITransport> CreateSteamTransport() { return std::make_unique<SteamTransport>(); }

}  // namespace qd::net
