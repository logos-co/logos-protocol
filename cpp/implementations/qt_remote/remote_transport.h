#ifndef REMOTE_TRANSPORT_H
#define REMOTE_TRANSPORT_H

#include "../../logos_transport.h"
#include "../../logos_object.h"
#include <QString>

class QRemoteObjectRegistryHost;
class QRemoteObjectNode;

class RemoteTransportHost : public LogosTransportHost {
public:
    explicit RemoteTransportHost(const QString& registryUrl);
    ~RemoteTransportHost() override;

    bool publishObject(const QString& name, QObject* object) override;
    void unpublishObject(const QString& name) override;

private:
    QRemoteObjectRegistryHost* m_registryHost;
    QString m_registryUrl;
};

class RemoteTransportConnection : public LogosTransportConnection {
public:
    explicit RemoteTransportConnection(const QString& registryUrl);
    ~RemoteTransportConnection() override;

    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;

    // Test hook: how many times requestObject() acquired a fresh replica
    // (process-wide). Lets a test assert the consumer's handle cache reuses one
    // replica instead of re-acquiring per call.
    static long acquireCount();
    static void resetAcquireCount();

private:
    bool connectToRegistry();

    QRemoteObjectNode* m_node;
    // Does the registry endpoint currently have a listener?
    //
    // Separate from m_connected because connectToNode() cannot answer it: it
    // returns false only for an unregistered URL SCHEME and never touches the
    // peer, so m_connected records "we attempted a connection", nothing more.
    // For `local:` URLs this probes the socket / named pipe directly; for any
    // other scheme it returns true, leaving those transports' behaviour
    // unchanged.
    bool endpointHasListener() const;

    QString m_registryUrl;
    bool m_connected;
};

#endif // REMOTE_TRANSPORT_H
