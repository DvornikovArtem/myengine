// NetLink.h
// The datagram layer under the reliable transport: something that sends and receives packets to addresses, nothing more.
//   LoopbackHub + LoopbackLink  packets move between endpoints of one process (the tests)
//   UdpLink                     winsock, non-blocking (two windows on one PC, a LAN)
//   SimLink                     wraps any link and adds latency, jitter, losses and duplicates to what it sends
// An address is an opaque 64-bit number: for UDP it is ip << 16 | port (host byte order), for the loopback the endpoint number.

#pragma once

#include <rl/net/NetTransport.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rl::net
{
    using NetAddress = uint64_t;

    class IDatagramLink
    {
    public:
        virtual ~IDatagramLink() = default;

        virtual bool Bind(uint16_t port, std::string& error) = 0;  // port 0 = any
        virtual uint16_t LocalPort() const = 0;
        // Resolves "ip:port" (or a name known to the link) into an address.
        virtual bool Resolve(const std::string& text, NetAddress& out, std::string& error) = 0;
        virtual void Send(NetAddress to, const uint8_t* data, size_t size, double now) = 0;
        // false when nothing is waiting; `now` lets a link hold packets back
        virtual bool Receive(NetAddress& from, std::vector<uint8_t>& data, double now) = 0;
        virtual void Close() = 0;
    };

    // The "network" of the tests: endpoints are numbered from 1, a send puts the packet into the inbox of the target at once (the
    // delays come from SimLink on top).
    class LoopbackHub
    {
    public:
        struct Packet
        {
            NetAddress from = 0;
            std::vector<uint8_t> data;
        };

        // A port-like number: Bind(port) gives an endpoint that name; Bind(0) takes a free one.
        bool Register(uint16_t port, uint16_t& assigned);
        void Unregister(uint16_t port);
        void Deliver(uint16_t to, NetAddress from, const uint8_t* data, size_t size);
        bool Take(uint16_t port, Packet& out);

    private:
        std::map<uint16_t, std::deque<Packet>> inboxes_;
        uint16_t nextFree_ = 40000;
    };

    std::unique_ptr<IDatagramLink> MakeLoopbackLink(LoopbackHub& hub);
    std::unique_ptr<IDatagramLink> MakeUdpLink();
    // Wraps `inner`: everything sent goes through the delay, loss and duplication of `params`; `seed` makes a test repeatable.
    std::unique_ptr<IDatagramLink> MakeSimLink(std::unique_ptr<IDatagramLink> inner, const NetSimParams& params, uint64_t seed);
}
