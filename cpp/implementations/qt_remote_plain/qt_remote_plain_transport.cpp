#include "qt_remote_plain_transport.h"

#include "../../logos_async_dispatch.h"
#include "../../logos_call_error.h"
#include "../../logos_object.h"
#include "../../logos_rpc_status.h"
#include "../../logos_types.h"
#include "../plain/qvariant_rpc_value.h"
#include "../../module_proxy.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <optional>
#include <thread>
#include <utility>

namespace logos::qt_remote_plain {
namespace {

using plain::RpcList;
using plain::RpcMap;
using plain::RpcValue;

constexpr int kDefaultTimeoutMs = 20000;
// A zero-timeout acquire (the consumer's retry tick) waits no longer than this
// for what is already there; an absent peer costs one dial attempt.
constexpr std::chrono::milliseconds kImmediateWait{20};
// How long a peer that is listening gets to send its greeting.
constexpr std::chrono::milliseconds kGreetingWait{5000};

std::chrono::milliseconds timeoutFor(int timeoutMs)
{
    return std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : kDefaultTimeoutMs);
}

// Both directions go through a QDataStream round trip with the QtRO stream
// settings, so the adapter reproduces exactly what a QRemoteObjectHost or
// replica would put on the wire, including types the plain codec keeps opaque.
Variant fromQVariant(const QVariant& value)
{
    if (!value.isValid()) return {};
    if (value.metaType().hasRegisteredDataStreamOperators()) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_2);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << value;
        if (stream.status() == QDataStream::Ok) {
            try {
                Reader reader(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                              static_cast<std::size_t>(bytes.size()));
                return reader.variantUntil(static_cast<std::size_t>(bytes.size()));
            } catch (const CodecError&) {
            }
        }
    }
    return Variant::fromRpc(plain::qvariantToRpcValue(value));
}

QVariant toQVariant(const Variant& value)
{
    if (value.type == MetaType::Invalid && !value.opaque) return {};
    try {
        Writer writer;
        writer.variant(value);
        const auto& data = writer.data();
        const QByteArray bytes(reinterpret_cast<const char*>(data.data()),
                               static_cast<qsizetype>(data.size()));
        QDataStream stream(bytes);
        stream.setVersion(QDataStream::Qt_6_2);
        stream.setByteOrder(QDataStream::LittleEndian);
        QVariant result;
        stream >> result;
        if (stream.status() == QDataStream::Ok) return result;
    } catch (const CodecError&) {
    }
    return value.opaque ? QVariant() : plain::rpcValueToQVariant(value.value);
}

QVariantList toQVariantList(const std::vector<Variant>& values)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) result.append(toQVariant(value));
    return result;
}

Variant invokePublishedDirect(QObject* object, bool handshake,
                              std::int32_t index, const std::vector<Variant>& args)
{
    QPointer<QObject> guarded(object);
    if (!guarded) return {};
    const QVariantList qargs = toQVariantList(args);
    if (handshake) {
        auto* proxy = qobject_cast<ModuleHandshakeProxy*>(guarded.data());
        if (!proxy || index != 0 || qargs.size() != 3) return {};
        return fromQVariant(proxy->informModuleToken(qargs[0].toString(),
                                                     qargs[1].toString(),
                                                     qargs[2].toString()));
    }
    auto* proxy = qobject_cast<ModuleProxy*>(guarded.data());
    if (!proxy) return {};
    switch (index) {
    case 0:
    case 1:
        if (qargs.size() < 2) return {};
        return fromQVariant(proxy->callRemoteMethod(
            qargs[0].toString(), qargs[1].toString(),
            qargs.size() >= 3 ? qargs[2].toList() : QVariantList{}));
    case 2:
        if (qargs.size() != 4) return {};
        return fromQVariant(proxy->callRemoteMethod(
            qargs[0].toString(), qargs[1].toString(), qargs[2].toList(),
            qargs[3].toString()));
    case 3:
        if (qargs.size() != 3) return {};
        return fromQVariant(proxy->informModuleToken(
            qargs[0].toString(), qargs[1].toString(), qargs[2].toString()));
    case 4: return fromQVariant(proxy->getPluginMethods());
    case 5: return fromQVariant(proxy->getPluginEvents());
    case 6: return fromQVariant(proxy->getPluginInterface());
    default: return {};
    }
}

class QtInvokeDispatcher {
    struct Pending {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
        bool canceled = false;
        Variant result;
    };

public:
    Variant invoke(QObject* object, bool handshake, std::int32_t index,
                   const std::vector<Variant>& args)
    {
        if (!object) return {};
        if (object->thread() == QThread::currentThread())
            return invokePublishedDirect(object, handshake, index, args);

        auto pending = std::make_shared<Pending>();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_active) return {};
            m_pending.push_back(pending);
        }
        const QPointer<QObject> guarded(object);
        const bool queued = QMetaObject::invokeMethod(object,
            [guarded, pending, handshake, index, args] {
                std::unique_lock<std::mutex> lock(pending->mutex);
                if (!pending->canceled)
                    pending->result = invokePublishedDirect(
                        guarded.data(), handshake, index, args);
                pending->done = true;
                lock.unlock();
                pending->changed.notify_all();
            }, Qt::QueuedConnection);
        if (!queued) {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->canceled = true;
            pending->done = true;
            pending->changed.notify_all();
        }

        std::unique_lock<std::mutex> lock(pending->mutex);
        pending->changed.wait(lock, [&] { return pending->done || pending->canceled; });
        const bool canceled = pending->canceled;
        Variant result = std::move(pending->result);
        lock.unlock();
        reap();
        return canceled ? Variant{} : result;
    }

    void cancel()
    {
        std::vector<std::shared_ptr<Pending>> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_active = false;
            for (auto& weak : m_pending)
                if (auto call = weak.lock()) pending.push_back(std::move(call));
            m_pending.clear();
        }
        for (const auto& call : pending) {
            std::unique_lock<std::mutex> lock(call->mutex);
            call->canceled = true;
            lock.unlock();
            call->changed.notify_all();
        }
    }

private:
    void reap()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
            [](const auto& weak) { return weak.expired(); }), m_pending.end());
    }

    std::mutex m_mutex;
    bool m_active = true;
    std::vector<std::weak_ptr<Pending>> m_pending;
};

struct HandleState {
    std::recursive_mutex callbackMutex;
    std::mutex mutex;
    std::condition_variable completionChanged;
    std::map<QString, std::vector<LogosObject::EventCallback>> callbacks;
    std::map<QString, QVariant> completions;
    bool active = true;
};

} // namespace

QtRemotePlainTransportHost::QtRemotePlainTransportHost(const QString& url)
{
    qRegisterMetaType<LogosResult>("LogosResult");
    std::string error;
    if (!m_server.start(url.toStdString(), &error))
        qCritical() << "QtRemotePlainTransportHost:" << error.c_str();
}

QtRemotePlainTransportHost::~QtRemotePlainTransportHost()
{
    for (const auto& entry : m_eventConnections) QObject::disconnect(entry.second);
    for (const auto& entry : m_destroyConnections) QObject::disconnect(entry.second);
    for (const auto& entry : m_cancelInvocations) entry.second();
    m_server.stop();
}

bool QtRemotePlainTransportHost::publishObject(const QString& name, QObject* object)
{
    if (!object || !m_server.isRunning()) return false;
    const bool handshake = qobject_cast<ModuleHandshakeProxy*>(object) != nullptr;
    if (!handshake && !qobject_cast<ModuleProxy*>(object)) return false;
    const std::string key = name.toStdString();
    auto dispatcher = std::make_shared<QtInvokeDispatcher>();
    Server::Object published{
        key,
        handshake ? moduleHandshakeProxyDefinition() : moduleProxyDefinition(),
        [guarded = QPointer<QObject>(object), handshake, dispatcher](
            std::int32_t index, const std::vector<Variant>& args) {
            return dispatcher->invoke(guarded.data(), handshake, index, args);
        }};
    std::string error;
    if (!m_server.publish(std::move(published), &error)) {
        qWarning() << "QtRemotePlainTransportHost: publish failed:" << error.c_str();
        return false;
    }
    m_cancelInvocations[key] = [dispatcher] { dispatcher->cancel(); };
    m_destroyConnections[key] = QObject::connect(
        object, &QObject::destroyed, [dispatcher] { dispatcher->cancel(); });
    if (!handshake) {
        auto* proxy = qobject_cast<ModuleProxy*>(object);
        m_eventConnections[key] = QObject::connect(
            proxy, &ModuleProxy::eventResponse, proxy,
            [this, key](const QString& eventName, const QVariantList& data) {
                Variant list = fromQVariant(data);
                std::string error;
                (void)m_server.emitSignal(key, 0,
                    {fromQVariant(eventName), std::move(list)}, &error);
            });
    }
    return true;
}

void QtRemotePlainTransportHost::unpublishObject(const QString& name)
{
    const std::string key = name.toStdString();
    if (auto it = m_cancelInvocations.find(key); it != m_cancelInvocations.end()) {
        it->second();
        m_cancelInvocations.erase(it);
    }
    if (auto it = m_destroyConnections.find(key); it != m_destroyConnections.end()) {
        QObject::disconnect(it->second);
        m_destroyConnections.erase(it);
    }
    if (auto it = m_eventConnections.find(key); it != m_eventConnections.end()) {
        QObject::disconnect(it->second);
        m_eventConnections.erase(it);
    }
    m_server.unpublish(key);
}

namespace {

// A blocking call made on a thread with a Qt event loop keeps the loop
// turning, as QRemoteObjectPendingCall::waitForFinished does, so a call back
// into this module (A -> B -> A) is served meanwhile instead of deadlocking.
template <typename Fn>
auto withEventsFlowing(Fn&& fn) -> decltype(fn())
{
    if (!QCoreApplication::instance() || !QThread::currentThread()->eventDispatcher())
        return fn();
    std::optional<decltype(fn())> result;
    QEventLoop loop;
    std::thread worker([&] {
        result.emplace(fn());
        QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
    });
    loop.exec(QEventLoop::ExcludeUserInputEvents | QEventLoop::WaitForMoreEvents);
    worker.join();
    return std::move(*result);
}

} // namespace

struct QtRemotePlainTransportConnection::Shared {
    std::shared_ptr<Client> client = std::make_shared<Client>();
    std::mutex mutex;
    std::unordered_map<std::string, std::vector<std::weak_ptr<HandleState>>> handles;
    // Set by connectToHost(), as qt_remote's node is by connectToNode().
    std::atomic<bool> armed{false};
    // Client::connect() drops a live connection, so one dial at a time.
    std::mutex dialMutex;
};

namespace {

// Dials unless connected. A listenerWait of 0 is a single attempt.
bool dial(QtRemotePlainTransportConnection::Shared& shared, const std::string& url,
          std::chrono::milliseconds listenerWait, std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> lock(shared.dialMutex);
    if (shared.client->isConnected()) return true;
    std::string error;
    return shared.client->connect(url, timeout, &error, nullptr, listenerWait);
}

} // namespace

namespace {

class QtRemotePlainLogosObject final : public LogosObject, public LogosObjectErrorChannel {
public:
    QtRemotePlainLogosObject(std::shared_ptr<QtRemotePlainTransportConnection::Shared> shared,
                             std::string object, std::shared_ptr<HandleState> state)
        : m_shared(std::move(shared)), m_object(std::move(object)), m_state(std::move(state)) {}

    QVariant callMethod(const QString& authToken, const QString& methodName,
                        const QVariantList& args, int timeoutMs) override
    {
        return callMethodWithError(authToken, methodName, args, timeoutMs, nullptr);
    }

    QVariant callMethodWithError(const QString& authToken, const QString& methodName,
                                 const QVariantList& args, int timeoutMs,
                                 logos::CallError* error) override
    {
        return withEventsFlowing([&] {
            return blockingCall(authToken, methodName, args, timeoutMs, error);
        });
    }

    QVariant blockingCall(const QString& authToken, const QString& methodName,
                          const QVariantList& args, int timeoutMs, logos::CallError* error)
    {
        if (error) error->clear();
        Variant wireArgs = fromQVariant(args);
        std::string why;
        auto result = m_shared->client->call(m_object,
            "callRemoteMethod(QString,QString,QVariantList)",
            {fromQVariant(authToken), fromQVariant(methodName), std::move(wireArgs)},
            timeoutFor(timeoutMs), &why);
        if (!result) {
            if (error) *error = logos::callErrorTransport(m_object, why);
            return {};
        }
        QVariant converted = toQVariant(*result);
        QString completionId;
        if (!logos::isPendingCallSentinel(converted, &completionId)) return converted;
        std::unique_lock<std::mutex> lock(m_state->mutex);
        if (!m_state->completionChanged.wait_for(lock, timeoutFor(timeoutMs), [&] {
                return !m_state->active || m_state->completions.count(completionId) != 0;
            })) {
            if (error) *error = logos::callErrorTimeout(
                m_object, methodName.toStdString(),
                static_cast<int>(timeoutFor(timeoutMs).count()));
            return {};
        }
        auto it = m_state->completions.find(completionId);
        if (it == m_state->completions.end()) return {};
        QVariant value = std::move(it->second);
        m_state->completions.erase(it);
        return value;
    }

    void callMethodAsync(const QString& authToken, const QString& methodName,
                         const QVariantList& args, int timeoutMs,
                         AsyncResultCallback callback) override
    {
        callMethodAsyncWithError(authToken, methodName, args, timeoutMs,
            [callback = std::move(callback)](QVariant value, const logos::CallError&) mutable {
                if (callback) callback(std::move(value));
            });
    }

    void callMethodAsyncWithError(const QString& authToken, const QString& methodName,
                                  const QVariantList& args, int timeoutMs,
                                  AsyncResultErrorCallback callback) override
    {
        if (!callback) return;
        auto shared = m_shared;
        auto state = m_state;
        const std::string object = m_object;
        std::thread([shared, state, object, authToken, methodName, args, timeoutMs,
                     callback = std::move(callback)]() mutable {
            QtRemotePlainLogosObject temporary(shared, object, state);
            logos::CallError error;
            QVariant value = temporary.callMethodWithError(
                authToken, methodName, args, timeoutMs, &error);
            auto deliver = [state, callback = std::move(callback),
                            value = std::move(value), error = std::move(error)]() mutable {
                std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->active) return;
                }
                if (callback) callback(std::move(value), error);
            };
            if (QCoreApplication* app = QCoreApplication::instance())
                QMetaObject::invokeMethod(app, std::move(deliver), Qt::QueuedConnection);
            else
                deliver();
        }).detach();
    }

    bool informModuleToken(const QString& authToken, const QString& moduleName,
                           const QString& token, int timeoutMs) override
    {
        return withEventsFlowing([&] {
            std::string error;
            auto result = m_shared->client->call(m_object,
                "informModuleToken(QString,QString,QString)",
                {fromQVariant(authToken), fromQVariant(moduleName), fromQVariant(token)},
                timeoutFor(timeoutMs), &error);
            return result && toQVariant(*result).toBool();
        });
    }

    void onEvent(const QString& eventName, EventCallback callback) override
    {
        if (!callback || logos::isReservedEventName(eventName)) return;
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->active) m_state->callbacks[eventName].push_back(std::move(callback));
    }

    void disconnectEvents() override
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->callbacks.clear();
    }

    void emitEvent(const QString&, const QVariantList&) override {}

    QJsonArray getMethods() override
    {
        return withEventsFlowing([&] {
            std::string error;
            auto result = m_shared->client->call(m_object, "getPluginMethods()", {},
                                                 timeoutFor(kDefaultTimeoutMs), &error);
            return result ? toQVariant(*result).toJsonArray() : QJsonArray{};
        });
    }

    void release() override
    {
        std::lock_guard<std::recursive_mutex> callbackLock(m_state->callbackMutex);
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->active = false;
            m_state->callbacks.clear();
        }
        m_state->completionChanged.notify_all();
        delete this;
    }

    quintptr id() const override { return reinterpret_cast<quintptr>(m_state.get()); }
    bool isValid() const override { return m_shared->client->isConnected(); }

private:
    std::shared_ptr<QtRemotePlainTransportConnection::Shared> m_shared;
    std::string m_object;
    std::shared_ptr<HandleState> m_state;
};

} // namespace

QtRemotePlainTransportConnection::QtRemotePlainTransportConnection(const QString& url)
    : m_shared(std::make_shared<Shared>()), m_url(url.toStdString())
{
    qRegisterMetaType<LogosResult>("LogosResult");
    std::weak_ptr<Shared> weak = m_shared;
    m_shared->client->setInternalEventHandler(
        [weak](const std::string& object, std::int32_t signal,
               const std::vector<Variant>& arguments) {
            auto shared = weak.lock();
            if (!shared || signal != 0 || arguments.size() != 2) return false;
            const QString eventName = toQVariant(arguments[0]).toString();
            if (!logos::isReservedEventName(eventName)) return false;
            const QVariantList data = toQVariant(arguments[1]).toList();
            if (data.size() != 2) return true;
            std::vector<std::shared_ptr<HandleState>> states;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                auto& entries = shared->handles[object];
                for (auto it = entries.begin(); it != entries.end();) {
                    if (auto state = it->lock()) { states.push_back(std::move(state)); ++it; }
                    else it = entries.erase(it);
                }
            }
            for (const auto& state : states) {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->active) continue;
                state->completions[data[0].toString()] = data[1];
                state->completionChanged.notify_all();
            }
            return true;
        });
    m_shared->client->setEventHandler(
        [weak](const std::string& object, std::int32_t signal,
               std::vector<Variant> arguments) {
            auto shared = weak.lock();
            if (!shared || signal != 0 || arguments.size() != 2) return;
            const QString eventName = toQVariant(arguments[0]).toString();
            const QVariantList data = toQVariant(arguments[1]).toList();
            std::vector<std::shared_ptr<HandleState>> states;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                auto& entries = shared->handles[object];
                for (auto it = entries.begin(); it != entries.end();) {
                    if (auto state = it->lock()) { states.push_back(std::move(state)); ++it; }
                    else it = entries.erase(it);
                }
            }
            for (const auto& state : states) {
                std::vector<LogosObject::EventCallback> callbacks;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->active) continue;
                    callbacks = state->callbacks[eventName];
                    const auto wildcard = state->callbacks[QString{}];
                    callbacks.insert(callbacks.end(), wildcard.begin(), wildcard.end());
                }
                for (const auto& callback : callbacks) {
                    try { callback(eventName, data); } catch (...) {}
                }
            }
        });
}

QtRemotePlainTransportConnection::~QtRemotePlainTransportConnection()
{
    m_shared->client->close();
}

// As qt_remote's connectToNode(): records the endpoint and returns. A peer that
// is not listening yet is dialled again by isConnected() and requestObject().
bool QtRemotePlainTransportConnection::connectToHost()
{
    if (m_url.empty()) return false;
    m_shared->armed = true;
    dial(*m_shared, m_url, std::chrono::milliseconds(0), kGreetingWait);
    return true;
}

// Truthful, as qt_remote's is: a peer that has come up since is dialled once.
bool QtRemotePlainTransportConnection::isConnected() const
{
    if (m_shared->client->isConnected()) return true;
    return m_shared->armed && dial(*m_shared, m_url, std::chrono::milliseconds(0), kGreetingWait);
}

bool QtRemotePlainTransportConnection::reconnect()
{
    m_shared->client->close();
    return connectToHost();
}

// timeoutMs bounds the dial and the acquire together; 0 waits for nothing that
// is not there already, as the consumer's retry tick requires.
LogosObject* QtRemotePlainTransportConnection::requestObject(const QString& objectName,
                                                             int timeoutMs)
{
    const auto budget = timeoutMs > 0 ? std::chrono::milliseconds(timeoutMs) : kImmediateWait;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto listenerWait = timeoutMs > 0 ? budget : std::chrono::milliseconds(0);
    const std::string object = objectName.toStdString();
    const auto acquire = [&] {
        if (!dial(*m_shared, m_url, listenerWait, budget)) return false;
        const auto left = std::max(std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
        std::string error;
        return m_shared->client->acquire(object, left, &error);
    };
    // A caller that waits keeps its thread's events flowing, as calls here and
    // qt_remote's acquire do, so a peer this thread serves can answer.
    if (!(timeoutMs > 0 ? withEventsFlowing(acquire) : acquire())) return nullptr;
    auto state = std::make_shared<HandleState>();
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->handles[object].push_back(state);
    }
    return new QtRemotePlainLogosObject(m_shared, object, std::move(state));
}

} // namespace logos::qt_remote_plain
