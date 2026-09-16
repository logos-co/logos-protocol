#ifndef LOGOS_PLAIN_TRANSPORT_CONNECTION_H
#define LOGOS_PLAIN_TRANSPORT_CONNECTION_H

#include "logos_transport.h"
#include "logos_transport_config.h"

#include "rpc_connection.h"

#include <memory>
#include <mutex>
#include <string>

namespace logos::plain {

// -----------------------------------------------------------------------------
// PlainTransportConnection — consumer-side LogosTransportConnection.
//
// connectToHost() opens a TCP (or TLS) socket to the daemon's endpoint from
// the LogosTransportConfig and starts the RPC read loop. requestObject()
// returns a PlainLogosObject sharing that connection.
//
// A connection the peer dropped stays dead, so requestObject() and
// isConnected() redial in the background and adopt the result (see Dial).
// connectToHost() waits on the same Dial, so its deadline bounds the first
// connect too, except on the io thread, which cannot run a Dial it waits on.
// -----------------------------------------------------------------------------
class PlainTransportConnection : public LogosTransportConnection {
public:
    explicit PlainTransportConnection(LogosTransportConfig cfg);
    ~PlainTransportConnection() override;

    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;
    QString endpointUrl(const QString& instanceId,
                        const QString& moduleName) override;

private:
    struct Dial;   // one connection attempt, run on the io thread; defined in the .cpp

    // The open connection, or null. Adopts a finished redial or starts one; waits at most `waitMs`.
    std::shared_ptr<RpcConnectionBase> liveConnection(int waitMs, bool waitForRunningDial) const;
    // Joins or starts a dial and waits out its deadline; the connection, or null and the reason.
    std::shared_ptr<RpcConnectionBase> awaitDial(std::string& why);

    LogosTransportConfig                       m_cfg;
    // Guards everything below: isConnected() is not marshalled to the owner thread.
    mutable std::mutex                         m_mu;
    mutable std::shared_ptr<RpcConnectionBase> m_conn;
    mutable std::shared_ptr<Dial>              m_dial;
    mutable bool                               m_connected = false;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_TRANSPORT_CONNECTION_H
