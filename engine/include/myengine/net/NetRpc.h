// NetRpc.h
// Remote procedure calls of the network subsystem (docs/network/architecture.md): a client asks the server to do something and always learns
// what came of it. A call carries a request number, a method number and its arguments; the server looks the method up, its handler checks
// the right of the caller and the arguments, and the reply (same request number) says Ok or why not. A call that gets no reply in time ends
// as Timeout on the client, so nothing is lost silently.
//
//   call   [call type][request u32][method u16][argument bytes u16][arguments]          sent reliable
//   reply  [reply type][request u32][method u16][status u8][text bytes u8][text]         sent reliable
//
// The type bytes belong to the protocol of the game (rl::RlMsg::RpcCall / RpcReply): the RPC layer only writes them first, so its messages
// travel next to the other messages of the game. Arguments are written with ByteWriter and read with ByteReader (little endian, bounds checked).

#pragma once

#include <myengine/net/NetTransport.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace myengine::net
{
    class ByteWriter
    {
    public:
        explicit ByteWriter(std::vector<uint8_t>& out)
            : out_(out)
        {
        }
        void U8(uint8_t v) { out_.push_back(v); }
        void U16(uint16_t v)
        {
            out_.push_back(static_cast<uint8_t>(v));
            out_.push_back(static_cast<uint8_t>(v >> 8));
        }
        void U32(uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
            }
        }
        void Bytes(const uint8_t* data, size_t size) { out_.insert(out_.end(), data, data + size); }

    private:
        std::vector<uint8_t>& out_;
    };

    // Reading past the end gives zeros and makes Ok() false, so a decoder reads everything and checks once
    class ByteReader
    {
    public:
        ByteReader(const uint8_t* data, size_t size)
            : data_(data)
            , size_(size)
        {
        }
        uint8_t U8() { return Take(1) ? data_[pos_ - 1] : 0; }
        uint16_t U16() { return Take(2) ? static_cast<uint16_t>(data_[pos_ - 2] | (data_[pos_ - 1] << 8)) : 0; }
        uint32_t U32()
        {
            if (!Take(4))
            {
                return 0;
            }
            const uint8_t* p = data_ + pos_ - 4;
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        const uint8_t* Bytes(size_t size) { return Take(size) ? data_ + pos_ - size : nullptr; }
        bool Ok() const { return ok_; }
        bool AtEnd() const { return pos_ == size_; }
        size_t Left() const { return size_ - pos_; }

    private:
        bool Take(size_t n)
        {
            if (!ok_ || size_ - pos_ < n)
            {
                ok_ = false;
                return false;
            }
            pos_ += n;
            return true;
        }
        const uint8_t* data_;
        size_t size_;
        size_t pos_ = 0;
        bool ok_ = true;
    };

    enum class RpcStatus : uint8_t
    {
        Ok = 0,
        UnknownMethod = 1,  // the server has no such method
        BadArguments = 2,   // the arguments did not decode or are out of range
        NotAllowed = 3,     // this caller may not call it (now)
        TooFast = 4,        // the caller calls it more often than allowed
        Timeout = 100,      // client side: no reply in time
        Closed = 101,       // client side: the connection ended before the reply
    };

    const char* RpcStatusName(RpcStatus status);  // "OK", "UNKNOWN METHOD", ...

    // What became of one call (the client gets one per call, whatever happened)
    struct RpcResult
    {
        uint32_t request = 0;
        uint16_t method = 0;
        std::string methodName;  // the name the caller gave it ("QuickChat"), or "#999"
        RpcStatus status = RpcStatus::Ok;
        std::string text;        // Ok: what the server says it did; otherwise why not
        double latencyMs = 0.0;
    };

    struct RpcCounters
    {
        uint64_t calls = 0;
        uint64_t ok = 0;
        uint64_t refused = 0;    // UnknownMethod / BadArguments / NotAllowed / TooFast
        uint64_t timeouts = 0;   // client
        uint64_t malformed = 0;  // a call (server) or a reply (client) that did not decode
        int pending = 0;         // client: calls still waiting for their reply
    };

    // The server side: methods by number. A handler reads its arguments from `args`, checks the caller and them, does the thing and returns Ok
    // (with what it did in `text`) or the reason of the refusal.
    class RpcServer
    {
    public:
        using Handler = std::function<RpcStatus(PeerId caller, ByteReader& args, std::string& text)>;

        void Register(uint16_t method, std::string name, Handler handler);
        void Clear();

        // A call message (its first byte is the call type of the game). Writes the reply, with `replyType` first, into `reply` and returns true;
        // false when the message is not a call at all (cut before the request number: there is nobody to answer), counted as malformed.
        bool Handle(PeerId caller, const uint8_t* data, size_t size, uint8_t replyType, std::vector<uint8_t>& reply);

        const RpcCounters& Counters() const { return counters_; }
        const std::string& LastLine() const { return last_; }  // "QuickChat #12 from peer 2: OK - ..."

    private:
        struct Method
        {
            std::string name;
            Handler handler;
        };
        std::map<uint16_t, Method> methods_;
        RpcCounters counters_;
        std::string last_;
    };

    // The client side: numbers the calls and pairs the replies with them.
    class RpcClient
    {
    public:
        explicit RpcClient(double timeoutSeconds = 3.0)
            : timeout_(timeoutSeconds)
        {
        }

        // Writes a call of `method` (`callType` first) into `out`; the caller sends it reliable. Returns the request number.
        uint32_t Call(uint8_t callType, uint16_t method, const std::string& name, const std::vector<uint8_t>& args, double now, std::vector<uint8_t>& out);
        // A reply message: true with the result of a call of ours; false when it did not decode or answers no call (counted as malformed)
        bool OnReply(const uint8_t* data, size_t size, double now, RpcResult& result);
        // Calls older than the timeout come out as Timeout
        void Expire(double now, std::vector<RpcResult>& out);
        // The connection ended: every call still waiting comes out as Closed
        void CloseAll(std::vector<RpcResult>& out);

        const RpcCounters& Counters() const { return counters_; }

    private:
        struct Pending
        {
            uint16_t method = 0;
            std::string name;
            double sent = 0.0;
        };
        double timeout_;
        uint32_t next_ = 1;
        std::map<uint32_t, Pending> pending_;
        RpcCounters counters_;
    };
}
