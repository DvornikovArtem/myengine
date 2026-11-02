// NetLog.h
// The diagnostics of the network subsystem: where its log lines go and the counting of messages that had to be thrown away.
// The engine points the sink at its logger (RlGameSystem); the tests collect the lines. Without a sink nothing is written.

#pragma once

#include <myengine/net/NetTransport.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace myengine::net
{
    enum class NetLogLevel
    {
        Info,
        Warning,
        Error,
    };

    using NetLogSink = std::function<void(NetLogLevel, const std::string&)>;

    // One sink for the process (the network runs on the game thread); an empty one turns the log off
    void SetNetLogSink(NetLogSink sink);
    void NetLog(NetLogLevel level, const std::string& text);

    // Messages that a host or a client received and could not use. Each one is counted; the first few of a kind and then every hundredth are
    // logged, so that a flood of garbage does not drown the log (or the frame).
    struct NetBadMessages
    {
        uint64_t unknown = 0;     // a type that nobody handles
        uint64_t malformed = 0;   // a known type whose bytes did not decode (cut, a number out of range)
        std::string last;         // the last line about it (the network panel shows it)

        // `who`: "host" / "client"; `type`: the first byte of the message
        void Unknown(const char* who, PeerId peer, const uint8_t* data, size_t size);
        void Malformed(const char* who, const char* what, PeerId peer, size_t size);
        uint64_t Total() const { return unknown + malformed; }
    };
}
