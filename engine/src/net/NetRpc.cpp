// NetRpc.cpp

#include <myengine/net/NetRpc.h>

#include <myengine/net/NetLog.h>

#include <algorithm>
#include <utility>

namespace myengine::net
{
    namespace
    {
        constexpr size_t kMaxReplyText = 200;

        void WriteReply(uint8_t replyType, uint32_t request, uint16_t method, RpcStatus status, const std::string& text, std::vector<uint8_t>& out)
        {
            out.clear();
            ByteWriter w(out);
            w.U8(replyType);
            w.U32(request);
            w.U16(method);
            w.U8(static_cast<uint8_t>(status));
            const size_t length = std::min(text.size(), kMaxReplyText);
            w.U8(static_cast<uint8_t>(length));
            w.Bytes(reinterpret_cast<const uint8_t*>(text.data()), length);
        }
    }

    const char* RpcStatusName(RpcStatus status)
    {
        switch (status)
        {
        case RpcStatus::Ok: return "OK";
        case RpcStatus::UnknownMethod: return "UNKNOWN METHOD";
        case RpcStatus::BadArguments: return "BAD ARGUMENTS";
        case RpcStatus::NotAllowed: return "NOT ALLOWED";
        case RpcStatus::TooFast: return "TOO FAST";
        case RpcStatus::Timeout: return "TIMEOUT";
        case RpcStatus::Closed: return "CONNECTION CLOSED";
        }
        return "?";
    }

    // ---- server ----------------------------------------------------------------------------------------------------------------------------

    void RpcServer::Register(uint16_t method, std::string name, Handler handler) { methods_[method] = Method{std::move(name), std::move(handler)}; }

    void RpcServer::Clear() { methods_.clear(); }

    bool RpcServer::Handle(PeerId caller, const uint8_t* data, size_t size, uint8_t replyType, std::vector<uint8_t>& reply)
    {
        ByteReader r(data, size);
        r.U8();  // the call type of the game
        const uint32_t request = r.U32();
        const uint16_t method = r.U16();
        const uint16_t argBytes = r.U16();
        if (!r.Ok())
        {
            ++counters_.malformed;
            last_ = "a broken RPC call from peer " + std::to_string(caller) + " (" + std::to_string(size) + " bytes): no request number to answer";
            NetLog(NetLogLevel::Warning, "net rpc: " + last_);
            return false;
        }
        ++counters_.calls;
        RpcStatus status = RpcStatus::Ok;
        std::string text;
        std::string name = "#" + std::to_string(method);
        const auto it = methods_.find(method);
        if (it != methods_.end())
        {
            name = it->second.name;
        }
        if (r.Left() != argBytes)
        {
            status = RpcStatus::BadArguments;
            text = "the call says " + std::to_string(argBytes) + " bytes of arguments, it has " + std::to_string(r.Left());
        }
        else if (it == methods_.end())
        {
            status = RpcStatus::UnknownMethod;
            text = "the server has no method " + std::to_string(method);
        }
        else
        {
            ByteReader args(data + (size - argBytes), argBytes);
            status = it->second.handler(caller, args, text);
            if (status == RpcStatus::Ok && (!args.Ok() || !args.AtEnd()))
            {
                status = RpcStatus::BadArguments;  // a handler that did not check what it read
                text = "the arguments of " + name + " did not decode";
            }
        }
        if (status == RpcStatus::Ok)
        {
            ++counters_.ok;
        }
        else
        {
            ++counters_.refused;
        }
        last_ = name + " #" + std::to_string(request) + " from peer " + std::to_string(caller) + ": " + RpcStatusName(status) + (text.empty() ? "" : " - " + text);
        NetLog(status == RpcStatus::Ok ? NetLogLevel::Info : NetLogLevel::Warning, "net rpc: " + last_);
        WriteReply(replyType, request, method, status, text, reply);
        return true;
    }

    // ---- client ----------------------------------------------------------------------------------------------------------------------------

    uint32_t RpcClient::Call(uint8_t callType, uint16_t method, const std::string& name, const std::vector<uint8_t>& args, double now, std::vector<uint8_t>& out)
    {
        const uint32_t request = next_++;
        out.clear();
        ByteWriter w(out);
        w.U8(callType);
        w.U32(request);
        w.U16(method);
        w.U16(static_cast<uint16_t>(args.size()));
        w.Bytes(args.data(), args.size());
        pending_[request] = Pending{method, name.empty() ? "#" + std::to_string(method) : name, now};
        ++counters_.calls;
        counters_.pending = static_cast<int>(pending_.size());
        return request;
    }

    bool RpcClient::OnReply(const uint8_t* data, size_t size, double now, RpcResult& result)
    {
        ByteReader r(data, size);
        r.U8();
        const uint32_t request = r.U32();
        const uint16_t method = r.U16();
        const uint8_t status = r.U8();
        const uint8_t length = r.U8();
        const uint8_t* text = r.Bytes(length);
        const auto it = pending_.find(request);
        if (!r.Ok() || !r.AtEnd() || it == pending_.end() || it->second.method != method || status > static_cast<uint8_t>(RpcStatus::TooFast))
        {
            ++counters_.malformed;
            NetLog(NetLogLevel::Warning, "net rpc: a reply (" + std::to_string(size) + " bytes) that answers no call of ours thrown away");
            return false;
        }
        result = RpcResult{};
        result.request = request;
        result.method = method;
        result.methodName = it->second.name;
        result.status = static_cast<RpcStatus>(status);
        result.text.assign(reinterpret_cast<const char*>(text), length);
        result.latencyMs = (now - it->second.sent) * 1000.0;
        pending_.erase(it);
        counters_.pending = static_cast<int>(pending_.size());
        if (result.status == RpcStatus::Ok)
        {
            ++counters_.ok;
        }
        else
        {
            ++counters_.refused;
        }
        return true;
    }

    void RpcClient::Expire(double now, std::vector<RpcResult>& out)
    {
        for (auto it = pending_.begin(); it != pending_.end();)
        {
            if (now - it->second.sent < timeout_)
            {
                ++it;
                continue;
            }
            RpcResult result;
            result.request = it->first;
            result.method = it->second.method;
            result.methodName = it->second.name;
            result.status = RpcStatus::Timeout;
            result.text = "no reply in " + std::to_string(static_cast<int>(timeout_)) + " s";
            result.latencyMs = (now - it->second.sent) * 1000.0;
            NetLog(NetLogLevel::Warning, "net rpc: " + result.methodName + " #" + std::to_string(result.request) + ": " + result.text);
            out.push_back(std::move(result));
            ++counters_.timeouts;
            it = pending_.erase(it);
        }
        counters_.pending = static_cast<int>(pending_.size());
    }

    void RpcClient::CloseAll(std::vector<RpcResult>& out)
    {
        for (const auto& entry : pending_)
        {
            RpcResult result;
            result.request = entry.first;
            result.method = entry.second.method;
            result.methodName = entry.second.name;
            result.status = RpcStatus::Closed;
            result.text = "the connection ended before the reply";
            out.push_back(std::move(result));
        }
        pending_.clear();
        counters_.pending = 0;
    }
}
