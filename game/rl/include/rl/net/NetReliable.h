// NetReliable.h
// The transport itself: a connection handshake, reliable ordered messages (sequence numbers, acks, retransmits), unreliable messages,
// keepalive and the round trip time, all on top of an IDatagramLink. The loopback and the UDP transport differ only in the link.
//
// A packet is  [magic 0xA7][type][token u32] + body. The token is made in the handshake and checked on every packet, so stray and old
// packets are dropped. The reliable message numbers are 16 bit; at most 128 of them are in flight, the rest wait in a queue.

#pragma once

#include <rl/net/NetLink.h>
#include <rl/net/NetTransport.h>

#include <memory>

namespace rl::net
{
    // `seed` makes the handshake numbers repeatable in the tests.
    std::unique_ptr<INetTransport> MakeReliableTransport(std::unique_ptr<IDatagramLink> link, uint64_t seed = 1);

    // Two ready-made transports. `sim` (when Active) adds latency, jitter, loss and duplicates to everything this transport sends.
    std::unique_ptr<INetTransport> MakeLoopbackTransport(LoopbackHub& hub, const NetSimParams& sim = {}, uint64_t seed = 1);
    std::unique_ptr<INetTransport> MakeUdpTransport(const NetSimParams& sim = {}, uint64_t seed = 1);
}
