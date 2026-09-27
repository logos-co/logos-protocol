#ifndef LOGOS_TRANSPORT_CONFIG_H
#define LOGOS_TRANSPORT_CONFIG_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// LogosTransportConfig — plain C++ value describing one transport endpoint.
//
// Intentionally Qt-free: the SDK is being de-Qt'd and new config types should
// not pull QtCore into consumers. See the transport backend implementations
// in cpp/implementations/ for how each protocol uses these fields.
// -----------------------------------------------------------------------------

// Append only: plugins built earlier hold these values in objects they share
// with the host in-process.
enum class LogosProtocol {
    LocalSocket,   // QLocalSocket via QRemoteObjects (existing code path)
    Tcp,           // removed in 0.15: reserved, and refused by both runtimes
    TcpSsl,        // removed in 0.15: reserved, and refused by both runtimes
    QtRemotePlain, // QtRO 2.0-compatible local IPC implemented without Qt
    Inproc,        // a provider in this process (plain runtime only; the Qt runtime refuses it)
    TlsTcp,        // mutual TLS 1.3 session between runtimes (plain runtime only; the Qt runtime refuses it)
    // Noise, Quic — future work
};

enum class LogosWireCodec {
    Json,   // nlohmann::json::dump / parse   (default for now)
    Cbor,   // nlohmann::json::to_cbor / from_cbor (future)
};

struct LogosTransportConfig {
    LogosProtocol protocol = LogosProtocol::LocalSocket;

    // TlsTcp: the address a provider binds, or a client dials.
    std::string host = "127.0.0.1";

    // 0 = the provider picks one (lp_provider_endpoints_json reports it).
    uint16_t port = 0;

    // Unused since 0.15, when TcpSsl went; kept so the layout does not change.
    // TlsTcp takes its credential and trust anchors through the C ABI.
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    bool verifyPeer = true;

    // Wire-format codec used for RPC framing on TlsTcp; LocalSocket and
    // QtRemotePlain use the QtRO wire profile.
    LogosWireCodec codec = LogosWireCodec::Json;
};

// Field-wise equality. Used as the equality predicate for hashed
// containers keyed by LogosTransportConfig (e.g. the explicit-transport
// LogosAPIClient cache in logos_api.h). Every field that can plausibly
// distinguish one transport-attached client from another belongs here —
// missing one would let two callers with different security or codec
// settings alias onto the same cached client.
inline bool operator==(const LogosTransportConfig& a,
                       const LogosTransportConfig& b) noexcept
{
    return a.protocol   == b.protocol
        && a.port       == b.port
        && a.verifyPeer == b.verifyPeer
        && a.codec      == b.codec
        && a.host       == b.host
        && a.caFile     == b.caFile
        && a.certFile   == b.certFile
        && a.keyFile    == b.keyFile;
}

inline bool operator!=(const LogosTransportConfig& a,
                       const LogosTransportConfig& b) noexcept
{
    return !(a == b);
}

using LogosTransportSet = std::vector<LogosTransportConfig>;

// -----------------------------------------------------------------------------
// Process-global default.
//
// Set once at startup (before constructing any LogosAPI). LogosAPI /
// LogosAPIClient consult this when no per-instance override is passed.
// Modules launched by a daemon inherit the daemon's default.
// -----------------------------------------------------------------------------
namespace LogosTransportConfigGlobal {

    inline LogosTransportConfig& defaultStorage() {
        static LogosTransportConfig cfg{};   // LocalSocket
        return cfg;
    }

    inline const LogosTransportConfig& getDefault() {
        return defaultStorage();
    }

    inline void setDefault(LogosTransportConfig cfg) {
        defaultStorage() = std::move(cfg);
    }

}

#endif // LOGOS_TRANSPORT_CONFIG_H
