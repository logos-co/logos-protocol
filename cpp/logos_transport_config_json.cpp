#include "logos_transport_config_json.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace logos {

namespace {

const char* protocolToString(LogosProtocol p)
{
    switch (p) {
    case LogosProtocol::LocalSocket: return "local";
    case LogosProtocol::QtRemotePlain: return "qt_remote_plain";
    case LogosProtocol::Tcp:         return "tcp";
    case LogosProtocol::TcpSsl:      return "tcp_ssl";
    case LogosProtocol::Inproc:      return "inproc";
    case LogosProtocol::TlsTcp:      return "tls_tcp";
    }
    return "local";
}

LogosProtocol protocolFromString(const std::string& s)
{
    if (s == "qt_remote_plain") return LogosProtocol::QtRemotePlain;
    if (s == "tcp")     return LogosProtocol::Tcp;
    if (s == "tcp_ssl") return LogosProtocol::TcpSsl;
    if (s == "inproc")  return LogosProtocol::Inproc;
    if (s == "tls_tcp") return LogosProtocol::TlsTcp;
    return LogosProtocol::LocalSocket;
}

const char* codecToString(LogosWireCodec c)
{
    switch (c) {
    case LogosWireCodec::Cbor: return "cbor";
    case LogosWireCodec::Json: return "json";
    }
    return "json";
}

LogosWireCodec codecFromString(const std::string& s)
{
    if (s == "cbor") return LogosWireCodec::Cbor;
    return LogosWireCodec::Json;
}

} // namespace

std::string transportSetToJsonString(const LogosTransportSet& set)
{
    json arr = json::array();
    for (const auto& cfg : set) {
        json o;
        o["protocol"] = protocolToString(cfg.protocol);
        if (cfg.protocol == LogosProtocol::TlsTcp) {
            o["host"]  = cfg.host;
            o["port"]  = cfg.port;
            o["codec"] = codecToString(cfg.codec);
        }
        arr.push_back(std::move(o));
    }
    return arr.dump();  // single-line, suitable for CLI / env-var passing
}

bool parseTransportSet(const std::string& jsonStr, LogosTransportSet* out, std::string* error)
{
    const auto fail = [error](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (!jsonStr.empty()) {
        const json arr = json::parse(jsonStr, nullptr, false);
        if (arr.is_discarded()) return fail("transport set is not JSON");
        if (!arr.is_array()) return fail("transport set is not a JSON array");
        for (const auto& o : arr) {
            if (!o.is_object()) return fail("transport entry is not an object: " + o.dump());
            for (const char* key : {"protocol", "host", "codec"})
                if (o.contains(key) && !o[key].is_string())
                    return fail(std::string("transport field '") + key + "' is not a string");
            if (o.contains("port")
                && (!o["port"].is_number_integer() || o["port"].get<long long>() < 0
                    || o["port"].get<long long>() > 0xFFFF))
                return fail("transport port is not 0..65535: " + o["port"].dump());
            const std::string protocol = o.value("protocol", std::string{"local"});
            if (protocol == "tcp" || protocol == "tcp_ssl")
                return fail("the " + protocol + " transport was removed in protocol 0.15: runtimes "
                            "reach each other over tls_tcp (peering)");
            if (protocol != "local" && protocol != "qt_remote_plain" && protocol != "inproc"
                && protocol != "tls_tcp")
                return fail("unknown transport protocol '" + protocol + "'");
            const std::string codec = o.value("codec", std::string{"json"});
            if (codec != "json" && codec != "cbor")
                return fail("unknown transport codec '" + codec + "'");
        }
    }
    if (out) *out = transportSetFromJsonString(jsonStr);
    return true;
}

LogosTransportSet transportSetFromJsonString(const std::string& jsonStr)
{
    LogosTransportSet out;
    if (jsonStr.empty()) return out;

    json arr;
    try {
        arr = json::parse(jsonStr);
    } catch (const json::exception&) {
        return out;
    }
    if (!arr.is_array()) return out;

    for (const auto& o : arr) {
        if (!o.is_object()) continue;
        LogosTransportConfig cfg;
        cfg.protocol = protocolFromString(o.value("protocol", std::string{"local"}));
        cfg.host = o.value("host", std::string{"127.0.0.1"});
        const int rawPort = o.value("port", 0);
        if (rawPort < 0 || rawPort > 0xFFFF) continue;
        cfg.port = static_cast<uint16_t>(rawPort);
        cfg.codec = codecFromString(o.value("codec", std::string{"json"}));
        out.push_back(std::move(cfg));
    }
    return out;
}

} // namespace logos
