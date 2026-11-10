// Knights' Domain — session contract (release flavour is Steam-only). Lobby + transport + liveness. Knows nothing about the game.
#pragma once
#include "qdcoop/net/Transport.h"

#include <functional>
#include <string>
#include <vector>

namespace qd::net {

enum class TransportKind : uint8_t { Steam, Udp };

enum class SessionState : uint8_t {
    Idle,           // not in a session
    CreatingLobby,  // Steam: CreateLobby call in flight
    Hosting,        // we are host, accepting peers
    JoiningLobby,   // Steam: JoinLobby call in flight
    Connecting,     // transport open, waiting for the host's first packet
    Connected,      // client connected to host
    Failed,         // see LastError(); call Leave() to return to Idle
};

struct SessionConfig {
    TransportKind kind = TransportKind::Steam;
    int           maxMembers = 4;          // including host
    std::string   modName;                 // lobby data "kd_mod"; friend lobbies are filtered by it; rich presence
                                           //       "status" = "<modName> co-op"
    std::string   modAuthor;               // lobby data "kd_author"
    std::string   modVersion;              // lobby data "kd_ver"
    std::string   gameBuild;               // written to lobby data "game_build"
    std::string   displayName;             // written to lobby data "host_name" when hosting
    uint16_t      udpListenPort = 0;       // Udp only (dev flavour): host listens here (default 27777 when 0 and hosting)
    int           timeoutMs = 10000;       // peer considered gone after this much silence (judged only inside Tick())
};

struct LobbyInfo {
    uint64_t    lobbyId = 0;
    PeerId      ownerId = kInvalidPeer;
    std::string hostName;
    std::string modName;      // kd_mod (always == SessionConfig::modName: other lobbies are filtered out)
    std::string modAuthor;    // kd_author
    std::string modVersion;   // kd_ver
    std::string gameBuild;
    int         members = 0;
    int         maxMembers = 0;
};

// Threading: every public method is game-thread only. Internally, Init() starts a keepalive thread that sends
// heartbeats to the connected peers whenever Tick() has not run for 1.5 s (e.g. the game thread is blocked by a level
// load); Shutdown() stops it. Every ITransport call (including calls made through Transport()) is serialised on an
// internal lock, so the transports never see two threads at once.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Init(const SessionConfig& cfg);   // creates the transport; Steam must already be initialized by the game
    void Shutdown();
    void Tick(float dt);                    // game thread, every frame: pumps API-call results, lobby members, heartbeats, timeouts, packets

    void Host();
    void JoinLobby(uint64_t lobbyId);                       // Steam
    void JoinUdp(const std::string& ipv4, uint16_t port);   // Udp (dev flavour; release: logs and does nothing)
    void Leave();

    void RefreshFriendLobbies();                            // Steam: friends in a lobby whose kd_mod == modName
    const std::vector<LobbyInfo>& FriendLobbies() const;

    // Steam overlay "Join Game" / invite accepted while the game runs; also parses "+connect_lobby <id>" from the command line at Init.
    // The session calls JoinLobby itself when this happens and Idle; otherwise it stores the id in PendingInviteLobby().
    uint64_t PendingInviteLobby() const;

    bool               IsHost() const;
    SessionState       State() const;
    const std::string& LastError() const;
    PeerId             LocalId() const;
    PeerId             HostId() const;
    std::vector<PeerId> Peers() const;      // connected remote peers (host: all clients; client: just the host)
    uint64_t           LobbyId() const;
    ITransport*        Transport();

    bool Send(PeerId to, Channel ch, const void* data, size_t size);
    void Broadcast(Channel ch, const void* data, size_t size, PeerId except = kInvalidPeer);

    // Callbacks run on the game thread inside Tick().
    std::function<void(PeerId)>        OnPeerConnected;
    std::function<void(PeerId)>        OnPeerDisconnected;
    std::function<void(const Packet&)> OnPacket;          // heartbeats are consumed internally and never surface here
    std::function<void(SessionState)>  OnStateChanged;

private:
    friend struct SessionTestAccess;   // net/SessionInternal.h (unit-test hooks)
    struct Impl;
    Impl* impl_;
};

}  // namespace qd::net
