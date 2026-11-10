// QD Coop — internal hooks of the Session. Not part of the public contract; used by tests/net only.
#pragma once
#include "qdcoop/net/Session.h"

#include <cstdint>

namespace qd::net {

struct SessionTestAccess {
    // Turns the keepalive thread's heartbeats on/off (default on). Off = a stalled Tick() really goes silent.
    static void SetKeepaliveEnabled(Session& s, bool enabled);
    // Heartbeats sent by the keepalive thread since the last Init().
    static uint32_t KeepaliveHeartbeats(const Session& s);
};

}  // namespace qd::net
