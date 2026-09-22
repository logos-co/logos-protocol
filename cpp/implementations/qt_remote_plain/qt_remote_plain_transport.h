#ifndef LOGOS_QT_REMOTE_PLAIN_TRANSPORT_H
#define LOGOS_QT_REMOTE_PLAIN_TRANSPORT_H

#include "../../logos_transport.h"
#include "qtro_transport.h"

#include <QMetaObject>
#include <QString>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace logos::qt_remote_plain {

// Qt-facing adapters over the Qt-free Client/Server.  They keep the installed
// LogosTransport interfaces intact for current Qt hosts while ensuring that
// socket I/O and QtRO serialization are performed by the plain implementation.
class QtRemotePlainTransportHost final : public LogosTransportHost {
public:
    explicit QtRemotePlainTransportHost(const QString& url);
    ~QtRemotePlainTransportHost() override;

    bool publishObject(const QString& name, QObject* object) override;
    void unpublishObject(const QString& name) override;

private:
    Server m_server;
    std::unordered_map<std::string, QMetaObject::Connection> m_eventConnections;
    std::unordered_map<std::string, QMetaObject::Connection> m_destroyConnections;
    std::unordered_map<std::string, std::function<void()>> m_cancelInvocations;
};

class QtRemotePlainTransportConnection final : public LogosTransportConnection {
public:
    struct Shared;

    explicit QtRemotePlainTransportConnection(const QString& url);
    ~QtRemotePlainTransportConnection() override;

    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;

private:
    std::shared_ptr<Shared> m_shared;
    std::string m_url;
};

} // namespace logos::qt_remote_plain

#endif
