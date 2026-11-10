// QD Coop — internal helpers of the UDP dev transport. Not part of the public contract.
#pragma once
#include "qdcoop/net/Transport.h"

namespace qd::net {

// Test hook: the UDP transport randomly drops this fraction (0..1) of every datagram it sends (data, resends and
// acks). Deterministic for a given seed. No-op if `transport` is not a UDP transport.
void SetUdpTestDropRate(ITransport* transport, double rate, uint32_t seed = 12345);

}  // namespace qd::net
