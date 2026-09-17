#ifndef REMOTE_TRANSPORT_H
#define REMOTE_TRANSPORT_H

#include "../../logos_transport.h"
#include "../../logos_object.h"
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>

class QRemoteObjectRegistryHost;
class QRemoteObjectNode;
class QRemoteObjectReplica;

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

    LogosObject* tryAcquireNow(const QString& objectName) override;

    // Test hook: how many times requestObject() acquired a fresh replica
    // (process-wide). Lets a test assert the consumer's handle cache reuses one
    // replica instead of re-acquiring per call.
    static long acquireCount();
    static void resetAcquireCount();

    // Test hook: facades a timed-out requestObject() parked for this name and
    // has not reused yet. Pins the bound — retrying against a module that is
    // down must park ONE facade, not one per attempt.
    int parkedCount(const QString& objectName) const;

private:
    bool connectToRegistry();

    // The two halves of "never free a facade QtRO still lists raw": park the
    // one a timeout gave up on, and hand it to the next wait for that name.
    void parkOrDelete(const QString& objectName, QRemoteObjectReplica* replica);
    QRemoteObjectReplica* takeParked(const QString& objectName);

    QRemoteObjectNode* m_node;
    // Parent of every in-flight PendingAcquire (defined in the .cpp). Destroying
    // it cancels them; it is reset explicitly at the TOP of the destructor so
    // pending replicas die before the node they belong to.
    QObject* m_pendingAcquires;
    // Parked probes for tryAcquireNow(), one per object name, each parented to
    // m_pendingAcquires so they die with it — BEFORE the node, in both the
    // destructor and reconnect().
    //
    // They are parked rather than freed because destroying a dynamic replica
    // whose shared implementation has not yet received the source's metaobject
    // leaves a DANGLING RAW POINTER inside that implementation: QtRO records
    // each such facade in QConnectedReplicaImplementation::m_parentsNeedingConnect
    // and ~QRemoteObjectReplica is an empty body that never deregisters. The
    // implementation then dereferences every entry when the class definition
    // arrives. Probing repeatedly and freeing each probe is therefore a
    // use-after-free with one dangling pointer per probe.
    QHash<QString, QPointer<QRemoteObjectReplica>> m_probes;
    // Facades requestObject() gave up on while they were still unsynced, parked
    // for the same reason as m_probes and reused by the next wait on that name.
    // Reused rather than merely parked because requestObject() is RETRIED: a
    // module that stays down would otherwise park one facade per attempt.
    QHash<QString, QList<QPointer<QRemoteObjectReplica>>> m_parked;
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
