// NetLog.cpp

#include <myengine/net/NetLog.h>

#include <cstdio>
#include <utility>

namespace myengine::net
{
    namespace
    {
        NetLogSink& Sink()
        {
            static NetLogSink sink;
            return sink;
        }

        bool Worth(uint64_t count) { return count <= 5 || count % 100 == 0; }
    }

    void SetNetLogSink(NetLogSink sink) { Sink() = std::move(sink); }

    void NetLog(NetLogLevel level, const std::string& text)
    {
        if (Sink())
        {
            Sink()(level, text);
        }
    }

    void NetBadMessages::Unknown(const char* who, PeerId peer, const uint8_t* data, size_t size)
    {
        ++unknown;
        char line[200];
        if (size == 0)
        {
            std::snprintf(line, sizeof(line), "net %s: an empty message from peer %u thrown away (%llu unknown so far)", who, peer, static_cast<unsigned long long>(unknown));
        }
        else
        {
            std::snprintf(line, sizeof(line), "net %s: unknown message type %u (%zu bytes) from peer %u thrown away (%llu unknown so far)", who, static_cast<unsigned>(data[0]), size, peer,
                          static_cast<unsigned long long>(unknown));
        }
        last = line;
        if (Worth(unknown))
        {
            NetLog(NetLogLevel::Warning, last);
        }
    }

    void NetBadMessages::Malformed(const char* who, const char* what, PeerId peer, size_t size)
    {
        ++malformed;
        char line[200];
        std::snprintf(line, sizeof(line), "net %s: a broken %s (%zu bytes) from peer %u thrown away (%llu broken so far)", who, what, size, peer, static_cast<unsigned long long>(malformed));
        last = line;
        if (Worth(malformed))
        {
            NetLog(NetLogLevel::Warning, last);
        }
    }
}
