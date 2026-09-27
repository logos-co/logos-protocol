#include "logos_transport_factory.h"
#include "logos_transport.h"
#include "logos_mode.h"
#include "logos_transport_config.h"
#include "implementations/qt_local/local_transport.h"
#include "implementations/qt_remote/remote_transport.h"
#include "implementations/qt_remote_plain/qt_remote_plain_transport.h"
#include "implementations/mock/mock_transport.h"

#include <QDebug>

namespace LogosTransportFactory {

namespace {

const char* refusal(LogosProtocol protocol)
{
    switch (protocol) {
    case LogosProtocol::Inproc: return "the Qt runtime cannot serve inproc";
    case LogosProtocol::TlsTcp: return "the Qt runtime cannot serve tls_tcp";
    default: return "tcp and tcp_ssl were removed in protocol 0.15";
    }
}

// A transport this runtime refuses: it never connects, and never falls back to another.
class RefusedTransportConnection : public LogosTransportConnection {
public:
    explicit RefusedTransportConnection(LogosProtocol protocol) : m_protocol(protocol) {}
    bool connectToHost() override
    {
        qCritical() << "LogosTransportFactory:" << refusal(m_protocol);
        return false;
    }
    bool isConnected() const override { return false; }
    bool reconnect() override { return connectToHost(); }
    LogosObject* requestObject(const QString&, int) override { return nullptr; }

private:
    LogosProtocol m_protocol;
};

// Windows runs QtRO only in its plain implementation, so the plain core never
// pairs with Qt's own there: every local protocol resolves to it, with no path
// back to the qt_remote backend.
LogosProtocol resolve(LogosProtocol protocol)
{
#ifdef _WIN32
    if (protocol != LogosProtocol::Tcp && protocol != LogosProtocol::TcpSsl
        && protocol != LogosProtocol::Inproc && protocol != LogosProtocol::TlsTcp)
        return LogosProtocol::QtRemotePlain;
#endif
    return protocol;
}

} // namespace

// Single resolution rule for both `createHost` overloads:
//   LogosMode::Mock                 → MockTransportHost   (cfg ignored)
//   LogosMode::Local                → LocalTransportHost  (cfg ignored)
//   LogosMode::Remote + LocalSocket → RemoteTransportHost (QRO); on Windows
//                                     QtRemotePlainTransportHost, see resolve()
//   LogosMode::Remote + anything else → refused (nullptr)
//
// Mode is consulted *first* so test fixtures setting Mock/Local always
// get the right transport regardless of which createHost overload (or
// LogosAPIProvider constructor) was used. The no-cfg overload below
// just delegates with `LogosTransportConfigGlobal::getDefault()` so
// there's exactly one path.
std::unique_ptr<LogosTransportHost>
createHost(const LogosTransportConfig& cfg, const QString& registryUrl)
{
    if (LogosModeConfig::isLocal()) {
        return std::make_unique<LocalTransportHost>();
    }
    if (LogosModeConfig::isMock()) {
        return std::make_unique<MockTransportHost>();
    }
    switch (resolve(cfg.protocol)) {
    case LogosProtocol::QtRemotePlain:
        return std::make_unique<logos::qt_remote_plain::QtRemotePlainTransportHost>(registryUrl);
    case LogosProtocol::Inproc:
    case LogosProtocol::TlsTcp:
    case LogosProtocol::Tcp:
    case LogosProtocol::TcpSsl:
        // Never another transport in its place.
        qCritical() << "LogosTransportFactory:" << refusal(cfg.protocol);
        return nullptr;
    case LogosProtocol::LocalSocket:
    default:
        return std::make_unique<RemoteTransportHost>(registryUrl);
    }
}

std::unique_ptr<LogosTransportHost> createHost(const QString& registryUrl)
{
    return createHost(LogosTransportConfigGlobal::getDefault(), registryUrl);
}

// Same resolution rule as createHost — see the comment block above.
std::unique_ptr<LogosTransportConnection>
createConnection(const LogosTransportConfig& cfg, const QString& registryUrl)
{
    if (LogosModeConfig::isLocal()) {
        return std::make_unique<LocalTransportConnection>();
    }
    if (LogosModeConfig::isMock()) {
        return std::make_unique<MockTransportConnection>();
    }
    switch (resolve(cfg.protocol)) {
    case LogosProtocol::QtRemotePlain:
        return std::make_unique<logos::qt_remote_plain::QtRemotePlainTransportConnection>(registryUrl);
    case LogosProtocol::Tcp:
    case LogosProtocol::TcpSsl:
    case LogosProtocol::Inproc:
    case LogosProtocol::TlsTcp:
        return std::make_unique<RefusedTransportConnection>(cfg.protocol);
    case LogosProtocol::LocalSocket:
    default:
        return std::make_unique<RemoteTransportConnection>(registryUrl);
    }
}

std::unique_ptr<LogosTransportConnection> createConnection(const QString& registryUrl)
{
    return createConnection(LogosTransportConfigGlobal::getDefault(), registryUrl);
}

// Same resolution rule as createConnection, answering "does the connection this
// cfg resolves to have to live on a thread with a Qt event loop?".
//   Local      → LocalTransportConnection: invokes in-process QObjects owned by
//                the module's main thread.                            → yes
//   Mock       → MockTransportConnection: no Qt objects, no sockets.  → no
//   LocalSocket→ RemoteTransportConnection: QRemoteObjectNode + QLocalSocket;
//                acquire and reply delivery both need the owner's loop. → yes
//   refused    → RefusedTransportConnection: never connects.          → no
bool needsQtEventLoop(const LogosTransportConfig& cfg)
{
    if (LogosModeConfig::isLocal()) return true;
    if (LogosModeConfig::isMock()) return false;
    switch (resolve(cfg.protocol)) {
    case LogosProtocol::QtRemotePlain:
        return false;
    case LogosProtocol::Tcp:
    case LogosProtocol::TcpSsl:
    case LogosProtocol::Inproc:
    case LogosProtocol::TlsTcp:
        return false;
    case LogosProtocol::LocalSocket:
    default:
        return true;
    }
}

}
