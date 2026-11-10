// QD Coop — co-op runtime contract. Owns the Session and the roster; feature modules (PlayerSync, later EnemySync,
// WorldSync) subscribe to messages through this hub. Game thread only.
#pragma once
#include "qdcoop/net/Protocol.h"
#include "qdcoop/net/Session.h"

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace qd::coop {

struct Member {
    bool             present = false;
    uint8_t          slot = 0;               // 0 = host
    net::PeerId      peer = net::kInvalidPeer;
    std::string      name;
    bool             isLocal = false;
};

void Init();       // creates Session from config, registers the per-frame ticker; call from core::OnReady
void Shutdown();

net::Session& Session();
bool InSession();                            // Hosting with >=1 peer, or Connected
bool IsHost();                               // true when Idle too (solo player is the authority of their own world)
int  PlayerCount();                          // members present including local, >= 1
const std::array<Member, net::kMaxPlayers>& Members();
std::optional<uint8_t> LocalSlot();
std::optional<uint8_t> SlotOf(net::PeerId peer);

// Local display name for the active save slot (profiles.json), falls back to the Steam persona name.
const std::string& LocalName();
void SetLocalName(const std::string& utf8);   // persists + broadcasts NameChanged

// Message routing. Handlers get the sender's slot (host-relayed messages carry the original slot inside the payload).
using Handler = std::function<void(uint8_t fromSlot, const uint8_t* data, size_t size)>;
void On(net::MsgType type, Handler handler);
void SendToHost(net::Channel ch, const std::vector<uint8_t>& bytes);           // no-op when host
void Broadcast(net::Channel ch, const std::vector<uint8_t>& bytes);            // host: to all clients; client: to host
void SendToSlot(uint8_t slot, net::Channel ch, const std::vector<uint8_t>& bytes);

// Lifecycle notifications for feature modules.
std::function<void()>& OnSessionStarted();
std::function<void()>& OnSessionEnded();
void AddMemberListener(std::function<void(const Member&, bool joined)> fn);

}  // namespace qd::coop
