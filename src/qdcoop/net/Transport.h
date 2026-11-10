// QD Coop — transport contract.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qd::net {

// SteamID64 for the Steam transport; for UDP: (0x7F00'0000'0000'0000 | (ipv4 << 16) | port).
using PeerId = uint64_t;
constexpr PeerId kInvalidPeer = 0;

enum class Channel : uint8_t {
    Reliable   = 0,  // ordered + reliable
    Unreliable = 1,  // latest-wins state snapshots
};

struct Packet {
    PeerId               from = kInvalidPeer;
    Channel              channel = Channel::Reliable;
    std::vector<uint8_t> data;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool   Init() = 0;
    virtual void   Shutdown() = 0;
    // Thread: game thread only. Returns false if the payload was rejected (too big, peer closed, not initialized).
    // Max payload: 1100 bytes on Unreliable, 64 KiB on Reliable.
    virtual bool   Send(PeerId to, Channel ch, const void* data, size_t size) = 0;
    // Thread: game thread. Appends every received packet since the last call.
    virtual void   Poll(std::vector<Packet>& out) = 0;
    // Make sure a session with this peer exists / is accepted (Steam: pre-send so the session auto-accepts).
    virtual void   OpenPeer(PeerId peer) = 0;
    virtual void   ClosePeer(PeerId peer) = 0;
    virtual PeerId LocalId() const = 0;
    // Round-trip estimate in ms, or -1 if unknown.
    virtual int    PingMs(PeerId peer) const = 0;
};

std::unique_ptr<ITransport> CreateSteamTransport();
// Dev transport for two game instances on one PC. listenPort 0 = pick an ephemeral port.
std::unique_ptr<ITransport> CreateUdpTransport(uint16_t listenPort);
// Builds the synthetic PeerId the UDP transport uses for host:port.
PeerId MakeUdpPeerId(const std::string& ipv4, uint16_t port);

}  // namespace qd::net
