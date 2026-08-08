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

class RemoteTransportConnection : public LogosTransportConnection,
                                  public LogosTransportAsyncAcquire {
public:
    explicit RemoteTransportConnection(const QString& registryUrl);
    ~RemoteTransportConnection() override;

    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;

    // Non-blocking acquire; see LogosTransportAsyncAcquire. Costs one pending
    // QRemoteObjectDynamicReplica and NO timer of ours — the node is already
    // retrying the endpoint every 250 ms whether or not anyone is waiting.
    bool requestObjectWhenAvailable(const QString& objectName,
                                    AcquireCallback onReady) override;

    // Test hook: how many times requestObject() acquired a fresh replica
    // (process-wide). Lets a test assert the consumer's handle cache reuses one
    // replica instead of re-acquiring per call.
    static long acquireCount();
    static void resetAcquireCount();

private:
    bool connectToRegistry();

    QRemoteObjectNode* m_node;
    // Parent of every in-flight PendingAcquire (defined in the .cpp). Destroying
    // it cancels them; it is reset explicitly at the TOP of the destructor so
    // pending replicas die before the node they belong to.
    QObject* m_pendingAcquires;
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
