#ifndef LOGOS_PLAIN_LOGOS_OBJECT_H
#define LOGOS_PLAIN_LOGOS_OBJECT_H

#include "logos_object.h"

#include "rpc_connection.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// PlainLogosObject — consumer-side LogosObject backed by the plain-C++
// RPC runtime. Identical public shape to LocalLogosObject / RemoteLogosObject
// so LogosAPIConsumer doesn't care which backend it's talking to.
//
// Owns a shared_ptr<RpcConnectionBase>; the transport layer hands the
// connection over after opening the socket. release() stops the connection.
// -----------------------------------------------------------------------------
class PlainLogosObject : public LogosObject {
public:
    PlainLogosObject(std::string objectName,
                     std::shared_ptr<RpcConnectionBase> conn);
    ~PlainLogosObject() override;

    QVariant callMethod(const QString& authToken,
                        const QString& methodName,
                        const QVariantList& args,
                        int timeoutMs) override;

    void callMethodAsync(const QString& authToken,
                         const QString& methodName,
                         const QVariantList& args,
                         int timeoutMs,
                         AsyncResultCallback callback) override;

    bool informModuleToken(const QString& authToken,
                           const QString& moduleName,
                           const QString& token,
                           int timeoutMs) override;

    void onEvent(const QString& eventName, EventCallback callback) override;
    void disconnectEvents() override;
    void emitEvent(const QString& eventName, const QVariantList& data) override;
    QJsonArray getMethods() override;
    void release() override;
    quintptr id() const override;

private:
    // Deferred ("multi") completion rendezvous. A multi provider returns a
    // pending sentinel (logos::pendingCallKey) from callMethod and later pushes
    // the real result as a logos::callCompleteEvent event keyed by callId. We
    // subscribe to that event EAGERLY (before any call can defer) so a completion
    // racing ahead of the waiter is buffered, then block the caller until the
    // matching callId lands. The completion arrives on the connection's IO
    // thread; the caller waits on another thread — m_completionMu/Cv bridge them.
    void ensureCompletionSub();
    QVariant awaitCompletion(const QString& callId, int timeoutMs);

    std::string                          m_objectName;
    std::shared_ptr<RpcConnectionBase>   m_conn;
    std::mutex                           m_mu;
    std::vector<std::pair<QString, EventCallback>> m_subs;

    std::mutex                           m_completionMu;
    std::condition_variable              m_completionCv;
    std::map<QString, QVariant>          m_completions;
    bool                                 m_completionSubscribed = false;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_LOGOS_OBJECT_H
