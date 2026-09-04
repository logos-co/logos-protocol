#ifndef LOCAL_TRANSPORT_H
#define LOCAL_TRANSPORT_H

#include "../../logos_transport.h"
#include "../../logos_object.h"

class ModuleProxy;

class LocalTransportHost : public LogosTransportHost {
public:
    bool publishObject(const QString& name, QObject* object) override;
    void unpublishObject(const QString& name) override;
};

class LocalTransportConnection : public LogosTransportConnection,
                                 public LogosTransportPresence {
public:
    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;

    // The only transport that can prove ABSENCE: the plugin registry it looks
    // in is the whole world here, so a miss is authoritative rather than
    // "not yet".
    TargetPresence targetPresence(const QString& objectName) override;
};

#endif // LOCAL_TRANSPORT_H
