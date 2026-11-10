// QD Coop — UDP dev transport: two game instances on one PC (or a LAN), no Steam needed.
//
// Datagram = [u32 magic 'QDCP'][u8 channel][u16 seq][u16 ack][u32 ackBits] + payload   (13-byte header, LE)
//   channel 0    reliable segment. payload = [u8 more][bytes]; a message > kSegmentBytes is split into several
//                consecutive segments (more = 1 on all but the last), reassembled in order by the receiver.
//   channel 1    unreliable; delivered immediately, dropped if not newer than the newest seq seen from that peer.
//   channel 0xFF ack only (no payload).
// ack/ackBits always describe the reliable stream we RECEIVE from that peer:
//   ack     = recvNext - 1  (cumulative: every seq <= ack has arrived)
//   ackBits = bit i set => seq (ack + 2 + i) also arrived (selective ack of out-of-order segments above the hole)
// Sender keeps unacked segments, resends each after 200 ms, gives up (peer marked dead, silent) after 30 resends AND
// kGiveUpAfter since the first transmission. (A peer whose game thread is blocked by a level
// load keeps the session alive with keepalive heartbeats but cannot ack for 10+ s; 30 x 200 ms alone would kill the
// reliable stream after ~6 s. A truly dead peer is still caught by the Session timeout long before kGiveUpAfter.)
// RTT: smoothed (1/8) from segments acked after exactly one transmission (Karn).
//
// Threading: no thread-local state. The Session serialises every call (its keepalive thread may call Send while the game
// thread is blocked), so the per-peer state, sendBuf_ and the test RNG are never touched by two threads at once.
#include "qdcoop/net/UdpTransport.h"

#include "qdcoop/core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <random>
#include <unordered_map>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace qd::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint8_t  kMagic[4] = {'Q', 'D', 'C', 'P'};
constexpr size_t   kHeaderBytes = 4 + 1 + 2 + 2 + 4;
constexpr uint8_t  kChanReliable = 0;
constexpr uint8_t  kChanUnreliable = 1;
constexpr uint8_t  kChanAckOnly = 0xFF;
constexpr size_t   kMaxUnreliablePayload = 1100;
constexpr size_t   kMaxReliablePayload = 64 * 1024;
constexpr size_t   kSegmentBytes = 1024;        // reliable bytes per datagram (plus the 1-byte "more" flag)
constexpr uint16_t kSendWindow = 64;            // max segments in flight per peer
constexpr uint16_t kRecvWindow = 256;           // how far ahead of recvNext we buffer
constexpr size_t   kMaxQueuedSegments = 8192;   // ~8 MiB of backlog per peer before Send() starts refusing
constexpr int      kMaxResends = 30;
constexpr auto     kResendAfter = std::chrono::milliseconds(200);
constexpr auto     kGiveUpAfter = std::chrono::seconds(120);
constexpr size_t   kRecvBufferBytes = 2048;
constexpr uint64_t kUdpPeerTag = 0x7F00'0000'0000'0000ull;
constexpr uint64_t kUdpPeerTagMask = 0xFF00'0000'0000'0000ull;

bool SeqNewer(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0; }

void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void PutU32(uint8_t* p, uint32_t v) {
    PutU16(p, static_cast<uint16_t>(v));
    PutU16(p + 2, static_cast<uint16_t>(v >> 16));
}
uint16_t GetU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(GetU16(p)) | (static_cast<uint32_t>(GetU16(p + 2)) << 16);
}

PeerId MakePeer(uint32_t ipv4HostOrder, uint16_t port) {
    return kUdpPeerTag | (static_cast<uint64_t>(ipv4HostOrder) << 16) | port;
}
bool IsUdpPeer(PeerId p) { return (p & kUdpPeerTagMask) == kUdpPeerTag; }

PeerId PeerFromAddr(const sockaddr_in& a) { return MakePeer(ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)); }

sockaddr_in AddrFromPeer(PeerId p) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(p & 0xFFFF));
    a.sin_addr.s_addr = htonl(static_cast<uint32_t>((p >> 16) & 0xFFFF'FFFFull));
    return a;
}

struct Segment {
    uint16_t             seq = 0;
    std::vector<uint8_t> payload;  // [more][bytes]
    Clock::time_point    firstSent{};
    Clock::time_point    lastSent{};
    int                  sends = 0;
    bool                 acked = false;
};

struct Peer {
    sockaddr_in addr{};
    bool        dead = false;  // gave up on reliable delivery; ignored until ClosePeer/OpenPeer

    // reliable, sending side
    uint16_t                         nextSeq = 0;
    std::deque<Segment>              inflight;  // ordered by seq; front = oldest
    std::deque<std::vector<uint8_t>> queued;    // segments waiting for window space

    // reliable, receiving side
    uint16_t                                          recvNext = 0;
    std::unordered_map<uint16_t, std::vector<uint8_t>> outOfOrder;
    std::vector<uint8_t>                              assembling;
    bool                                              assemblingOverflow = false;
    bool                                              ackPending = false;

    // unreliable
    uint16_t uSendSeq = 0;
    bool     uRecvAny = false;
    uint16_t uRecvLatest = 0;

    double srttMs = -1.0;
};

class UdpTransport final : public ITransport {
public:
    explicit UdpTransport(uint16_t listenPort) : port_(listenPort) {}
    ~UdpTransport() override { Shutdown(); }

    bool Init() override {
        if (sock_ != INVALID_SOCKET) return true;
        WSADATA wsa{};
        if (const int e = WSAStartup(MAKEWORD(2, 2), &wsa); e != 0) {
            QD_ERROR("net", "udp: WSAStartup failed ({})", e);
            return false;
        }
        wsaStarted_ = true;

        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            QD_ERROR("net", "udp: socket() failed ({})", WSAGetLastError());
            Shutdown();
            return false;
        }

        int bufBytes = 1 << 20;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufBytes), sizeof(bufBytes));
        setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufBytes), sizeof(bufBytes));

        // Windows reports ICMP "port unreachable" as WSAECONNRESET on the next recvfrom; turn that off.
        BOOL  reportReset = FALSE;
        DWORD ignored = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &ignored, nullptr, nullptr);

        u_long nonBlocking = 1;
        if (ioctlsocket(sock_, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            QD_ERROR("net", "udp: FIONBIO failed ({})", WSAGetLastError());
            Shutdown();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (bind(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            QD_ERROR("net", "udp: bind to port {} failed ({})", port_, WSAGetLastError());
            Shutdown();
            return false;
        }

        sockaddr_in bound{};
        int         len = sizeof(bound);
        if (getsockname(sock_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) port_ = ntohs(bound.sin_port);
        local_ = MakePeer(0x7F00'0001u, port_);
        QD_INFO("net", "udp: listening on port {}", port_);
        return true;
    }

    void Shutdown() override {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            QD_INFO("net", "udp: closed port {}", port_);
        }
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
        peers_.clear();
        local_ = kInvalidPeer;
    }

    bool Send(PeerId to, Channel ch, const void* data, size_t size) override {
        if (sock_ == INVALID_SOCKET || !data || size == 0) return false;
        auto it = peers_.find(to);
        if (it == peers_.end() || it->second.dead) return false;
        Peer& p = it->second;

        if (ch == Channel::Unreliable) {
            if (size > kMaxUnreliablePayload) return false;
            SendDatagram(p, kChanUnreliable, p.uSendSeq++, static_cast<const uint8_t*>(data), size);
            return true;
        }
        if (ch != Channel::Reliable || size > kMaxReliablePayload) return false;

        const size_t segments = (size + kSegmentBytes - 1) / kSegmentBytes;
        if (p.queued.size() + segments > kMaxQueuedSegments) {
            QD_WARN("net", "udp: reliable backlog to {:016x} is full, dropping a {}-byte message", to, size);
            return false;
        }
        const auto* src = static_cast<const uint8_t*>(data);
        for (size_t off = 0; off < size; off += kSegmentBytes) {
            const size_t         n = std::min(kSegmentBytes, size - off);
            std::vector<uint8_t> seg(n + 1);
            seg[0] = (off + n < size) ? 1 : 0;
            std::memcpy(seg.data() + 1, src + off, n);
            p.queued.push_back(std::move(seg));
        }
        FlushReliable(to, p, Clock::now());
        return true;
    }

    void Poll(std::vector<Packet>& out) override {
        if (sock_ == INVALID_SOCKET) return;
        const auto now = Clock::now();

        uint8_t buf[kRecvBufferBytes];
        for (int i = 0; i < 100000; ++i) {
            sockaddr_in from{};
            int         fromLen = sizeof(from);
            const int   n = recvfrom(sock_, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR) {
                const int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) break;
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // stray ICMP / oversized datagram: skip it
                QD_WARN("net", "udp: recvfrom failed ({})", e);
                break;
            }
            HandleDatagram(from, buf, static_cast<size_t>(n), out, now);
        }

        for (auto& [id, p] : peers_) {
            if (p.dead) continue;
            FlushReliable(id, p, now);
            if (!p.dead && p.ackPending) SendDatagram(p, kChanAckOnly, 0, nullptr, 0);
        }
    }

    void OpenPeer(PeerId peer) override {
        if (!IsUdpPeer(peer) || (peer & 0xFFFF) == 0) {
            QD_WARN("net", "udp: OpenPeer({:016x}) is not a UDP peer id", peer);
            return;
        }
        auto [it, inserted] = peers_.try_emplace(peer);
        if (!inserted && !it->second.dead) return;
        it->second = Peer{};
        it->second.addr = AddrFromPeer(peer);
        QD_INFO("net", "udp: opened peer {:016x}", peer);
    }

    void ClosePeer(PeerId peer) override {
        if (peers_.erase(peer)) QD_INFO("net", "udp: closed peer {:016x}", peer);
    }

    PeerId LocalId() const override { return local_; }

    int PingMs(PeerId peer) const override {
        auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.srttMs < 0) return -1;
        return static_cast<int>(std::lround(it->second.srttMs));
    }

    void SetDropRate(double rate, uint32_t seed) {
        dropRate_ = std::clamp(rate, 0.0, 1.0);
        rng_.seed(seed);
    }

private:
    uint32_t AckBits(const Peer& p) const {
        uint32_t bits = 0;
        if (p.outOfOrder.empty()) return bits;
        for (uint32_t i = 0; i < 32; ++i) {
            const uint16_t seq = static_cast<uint16_t>(p.recvNext + 1 + i);
            if (p.outOfOrder.contains(seq)) bits |= (1u << i);
        }
        return bits;
    }

    void SendDatagram(Peer& p, uint8_t chan, uint16_t seq, const uint8_t* payload, size_t n) {
        p.ackPending = false;  // this datagram carries our current ack state
        if (dropRate_ > 0.0 && dropDist_(rng_) < dropRate_) return;  // test hook: pretend the network ate it

        sendBuf_.resize(kHeaderBytes + n);
        uint8_t* d = sendBuf_.data();
        std::memcpy(d, kMagic, 4);
        d[4] = chan;
        PutU16(d + 5, seq);
        PutU16(d + 7, static_cast<uint16_t>(p.recvNext - 1));
        PutU32(d + 9, AckBits(p));
        if (n) std::memcpy(d + kHeaderBytes, payload, n);

        const int r = sendto(sock_, reinterpret_cast<const char*>(d), static_cast<int>(sendBuf_.size()), 0,
                             reinterpret_cast<const sockaddr*>(&p.addr), sizeof(p.addr));
        if (r == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) QD_DEBUG("net", "udp: sendto failed ({})", e);
            // treated like packet loss; reliable segments get resent
        }
    }

    void FlushReliable(PeerId id, Peer& p, Clock::time_point now) {
        for (Segment& s : p.inflight) {
            if (s.acked || now - s.lastSent < kResendAfter) continue;
            if (s.sends > kMaxResends && now - s.firstSent > kGiveUpAfter) {
                p.dead = true;
                QD_WARN("net", "udp: peer {:016x} did not ack seq {} after {} resends / {} s; giving up on it", id,
                        s.seq, s.sends - 1, std::chrono::duration_cast<std::chrono::seconds>(kGiveUpAfter).count());
                return;
            }
            SendDatagram(p, kChanReliable, s.seq, s.payload.data(), s.payload.size());
            s.lastSent = now;
            ++s.sends;
        }
        while (!p.queued.empty()) {
            const uint16_t span =
                p.inflight.empty() ? uint16_t{0} : static_cast<uint16_t>(p.nextSeq - p.inflight.front().seq);
            if (span >= kSendWindow) break;
            Segment s;
            s.seq = p.nextSeq++;
            s.payload = std::move(p.queued.front());
            p.queued.pop_front();
            s.firstSent = now;
            s.lastSent = now;
            s.sends = 1;
            SendDatagram(p, kChanReliable, s.seq, s.payload.data(), s.payload.size());
            p.inflight.push_back(std::move(s));
        }
    }

    void ProcessAck(Peer& p, uint16_t ack, uint32_t bits, Clock::time_point now) {
        for (Segment& s : p.inflight) {
            if (s.acked) continue;
            bool acked = static_cast<uint16_t>(ack - s.seq) < 0x8000;  // s.seq <= ack (serial arithmetic)
            if (!acked) {
                const uint16_t k = static_cast<uint16_t>(s.seq - ack - 2);
                acked = k < 32 && ((bits >> k) & 1u);
            }
            if (!acked) continue;
            s.acked = true;
            if (s.sends == 1) {
                const double sample = std::chrono::duration<double, std::milli>(now - s.lastSent).count();
                p.srttMs = p.srttMs < 0 ? sample : p.srttMs + (sample - p.srttMs) / 8.0;
            }
        }
        while (!p.inflight.empty() && p.inflight.front().acked) p.inflight.pop_front();
    }

    void DeliverSegment(PeerId id, Peer& p, const uint8_t* seg, size_t n, std::vector<Packet>& out) {
        const bool more = seg[0] != 0;
        if (!p.assemblingOverflow) {
            p.assembling.insert(p.assembling.end(), seg + 1, seg + n);
            if (p.assembling.size() > kMaxReliablePayload) {
                QD_WARN("net", "udp: reliable message from {:016x} exceeds {} bytes; discarding it", id,
                        kMaxReliablePayload);
                p.assembling.clear();
                p.assemblingOverflow = true;
            }
        }
        if (more) return;
        if (!p.assemblingOverflow && !p.assembling.empty()) {
            Packet pkt;
            pkt.from = id;
            pkt.channel = Channel::Reliable;
            pkt.data = std::move(p.assembling);
            out.push_back(std::move(pkt));
        }
        p.assembling.clear();
        p.assemblingOverflow = false;
    }

    void HandleDatagram(const sockaddr_in& from, const uint8_t* d, size_t n, std::vector<Packet>& out,
                        Clock::time_point now) {
        if (n < kHeaderBytes || std::memcmp(d, kMagic, 4) != 0 || from.sin_family != AF_INET) return;

        const PeerId id = PeerFromAddr(from);
        auto [it, inserted] = peers_.try_emplace(id);
        Peer& p = it->second;
        if (inserted) {
            p.addr = from;
            QD_INFO("net", "udp: first packet from unknown peer {:016x}; accepting it", id);
        }
        if (p.dead) return;

        const uint8_t  chan = d[4];
        const uint16_t seq = GetU16(d + 5);
        ProcessAck(p, GetU16(d + 7), GetU32(d + 9), now);

        const uint8_t* payload = d + kHeaderBytes;
        const size_t   pn = n - kHeaderBytes;

        if (chan == kChanUnreliable) {
            if (pn == 0) return;
            if (p.uRecvAny && !SeqNewer(seq, p.uRecvLatest)) return;  // stale or duplicate snapshot
            p.uRecvAny = true;
            p.uRecvLatest = seq;
            Packet pkt;
            pkt.from = id;
            pkt.channel = Channel::Unreliable;
            pkt.data.assign(payload, payload + pn);
            out.push_back(std::move(pkt));
            return;
        }
        if (chan != kChanReliable || pn == 0) return;  // ack-only or garbage

        p.ackPending = true;  // ack everything, duplicates included (their earlier ack may have been lost)
        const uint16_t dist = static_cast<uint16_t>(seq - p.recvNext);
        if (dist >= kRecvWindow) return;  // old duplicate
        if (dist != 0) {
            p.outOfOrder.try_emplace(seq, payload, payload + pn);
            return;
        }
        DeliverSegment(id, p, payload, pn, out);
        ++p.recvNext;
        for (auto f = p.outOfOrder.find(p.recvNext); f != p.outOfOrder.end(); f = p.outOfOrder.find(p.recvNext)) {
            DeliverSegment(id, p, f->second.data(), f->second.size(), out);
            p.outOfOrder.erase(f);
            ++p.recvNext;
        }
    }

    SOCKET                             sock_ = INVALID_SOCKET;
    bool                               wsaStarted_ = false;
    uint16_t                           port_ = 0;
    PeerId                             local_ = kInvalidPeer;
    std::unordered_map<PeerId, Peer>   peers_;
    std::vector<uint8_t>               sendBuf_;
    double                             dropRate_ = 0.0;
    std::mt19937                       rng_{12345};
    std::uniform_real_distribution<double> dropDist_{0.0, 1.0};
};

}  // namespace

std::unique_ptr<ITransport> CreateUdpTransport(uint16_t listenPort) {
    return std::make_unique<UdpTransport>(listenPort);
}

PeerId MakeUdpPeerId(const std::string& ipv4, uint16_t port) {
    if (port == 0) return kInvalidPeer;
    if (ipv4 == "localhost") return MakePeer(0x7F00'0001u, port);

    uint32_t ip = 0;
    size_t   i = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= ipv4.size() || ipv4[i] != '.') return kInvalidPeer;
            ++i;
        }
        uint32_t v = 0;
        int      digits = 0;
        while (i < ipv4.size() && ipv4[i] >= '0' && ipv4[i] <= '9') {
            v = v * 10 + static_cast<uint32_t>(ipv4[i] - '0');
            if (++digits > 3) return kInvalidPeer;
            ++i;
        }
        if (digits == 0 || v > 255) return kInvalidPeer;
        ip = (ip << 8) | v;
    }
    if (i != ipv4.size()) return kInvalidPeer;
    return MakePeer(ip, port);
}

void SetUdpTestDropRate(ITransport* transport, double rate, uint32_t seed) {
    if (auto* udp = dynamic_cast<UdpTransport*>(transport)) udp->SetDropRate(rate, seed);
}

}  // namespace qd::net
