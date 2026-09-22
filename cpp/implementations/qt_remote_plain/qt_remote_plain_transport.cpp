#include "qt_remote_plain_transport.h"

#include "../../logos_async_dispatch.h"
#include "../../logos_call_error.h"
#include "../../logos_object.h"
#include "../../logos_rpc_status.h"
#include "../../logos_types.h"
#include "../plain/qvariant_rpc_value.h"
#include "../../module_proxy.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <utility>

namespace logos::qt_remote_plain {
namespace {

using plain::RpcList;
using plain::RpcMap;
using plain::RpcValue;

constexpr int kDefaultTimeoutMs = 20000;

std::chrono::milliseconds timeoutFor(int timeoutMs)
{
    return std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : kDefaultTimeoutMs);
}

Variant fromQVariant(const QVariant& value)
{
    if (!value.isValid()) return {};

    const int logosResultId = QMetaType::fromName("LogosResult").id();
    if (logosResultId != QMetaType::UnknownType && value.userType() == logosResultId) {
        const LogosResult result = value.value<LogosResult>();
        return Variant::logosResult(result.success,
                                    fromQVariant(result.value),
                                    fromQVariant(result.error));
    }

    Variant out;
    out.isNull = value.isNull();
    out.value = plain::qvariantToRpcValue(value);
    switch (static_cast<QMetaType::Type>(value.userType())) {
    case QMetaType::Bool: out.type = MetaType::Bool; break;
    case QMetaType::Int:
    case QMetaType::Short:
    case QMetaType::Char:
    case QMetaType::SChar: out.type = MetaType::Int; break;
    case QMetaType::UInt:
    case QMetaType::UShort:
    case QMetaType::UChar: out.type = MetaType::UInt; break;
    case QMetaType::Long:
    case QMetaType::LongLong: out.type = MetaType::LongLong; break;
    case QMetaType::ULong:
    case QMetaType::ULongLong: out.type = MetaType::ULongLong; break;
    case QMetaType::Float:
    case QMetaType::Double: out.type = MetaType::Double; break;
    case QMetaType::QString: out.type = MetaType::String; break;
    case QMetaType::QStringList: out.type = MetaType::StringList; break;
    case QMetaType::QByteArray: out.type = MetaType::ByteArray; break;
    case QMetaType::QVariantList: out.type = MetaType::VariantList; break;
    case QMetaType::QVariantMap: out.type = MetaType::VariantMap; break;
    case QMetaType::QJsonValue: out.type = MetaType::JsonValue; break;
    case QMetaType::QJsonObject: out.type = MetaType::JsonObject; break;
    case QMetaType::QJsonArray: out.type = MetaType::JsonArray; break;
    case QMetaType::QJsonDocument: out.type = MetaType::JsonDocument; break;
    default: return Variant::fromRpc(std::move(out.value));
    }
    return out;
}

QJsonValue rpcToJson(const RpcValue& value)
{
    if (value.isNull()) return QJsonValue(QJsonValue::Null);
    if (value.isBool()) return value.asBool();
    if (value.isInt()) return static_cast<double>(value.asInt());
    if (value.isUInt()) return static_cast<double>(value.asUInt());
    if (value.isDouble()) return value.asDouble();
    if (value.isString()) return QString::fromStdString(value.asString());
    if (value.isList()) {
        QJsonArray out;
        for (const auto& child : value.asList().items) out.append(rpcToJson(child));
        return out;
    }
    if (value.isMap()) {
        QJsonObject out;
        for (const auto& entry : value.asMap().entries)
            out.insert(QString::fromStdString(entry.first), rpcToJson(entry.second));
        return out;
    }
    return QJsonValue(QJsonValue::Null);
}

QVariant toQVariant(const Variant& value)
{
    switch (value.type) {
    case MetaType::Invalid: return {};
    case MetaType::Bool: return value.value.asBool();
    case MetaType::Int: return static_cast<int>(value.value.asInt());
    case MetaType::UInt:
        return static_cast<uint>(value.value.isUInt() ? value.value.asUInt() : value.value.asInt());
    case MetaType::LongLong: return static_cast<qlonglong>(value.value.asInt());
    case MetaType::ULongLong:
        return static_cast<qulonglong>(value.value.isUInt() ? value.value.asUInt() : value.value.asInt());
    case MetaType::Double: return value.value.asDouble();
    case MetaType::String: return QString::fromStdString(value.value.asString());
    case MetaType::ByteArray: {
        const auto& bytes = value.value.asBytes().data;
        return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
    }
    case MetaType::StringList: {
        QStringList out;
        for (const auto& child : value.value.asList().items)
            out.append(QString::fromStdString(child.asString()));
        return out;
    }
    case MetaType::VariantList:
    case MetaType::VariantMap:
        return plain::rpcValueToQVariant(value.value);
    case MetaType::JsonValue: return QVariant::fromValue(rpcToJson(value.value));
    case MetaType::JsonObject: return QVariant::fromValue(rpcToJson(value.value).toObject());
    case MetaType::JsonArray: return QVariant::fromValue(rpcToJson(value.value).toArray());
    case MetaType::JsonDocument: {
        const QJsonValue json = rpcToJson(value.value);
        return QVariant::fromValue(json.isArray() ? QJsonDocument(json.toArray())
                                                  : QJsonDocument(json.toObject()));
    }
    case MetaType::User: {
        if (value.customType == "LogosResult" && value.value.isMap()) {
            const RpcMap& map = value.value.asMap();
            const RpcValue* success = map.find("success");
            const RpcValue* result = map.find("value");
            const RpcValue* error = map.find("error");
            if (success && result && error)
                return QVariant::fromValue(LogosResult{success->asBool(),
                    plain::rpcValueToQVariant(*result), plain::rpcValueToQVariant(*error)});
        }
        return {};
    }
    }
    return {};
}

QVariantList toQVariantList(const std::vector<Variant>& values)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) result.append(toQVariant(value));
    return result;
}

template <typename Fn>
auto onObjectThread(QObject* object, Fn&& fn) -> decltype(fn())
{
    using Result = decltype(fn());
    Result result{};
    if (!object) return result;
    if (object->thread() == QThread::currentThread()) return fn();
    const bool invoked = QMetaObject::invokeMethod(object, [&] { result = fn(); },
                                                    Qt::BlockingQueuedConnection);
    return invoked ? result : Result{};
}

Variant invokePublished(QObject* object, bool handshake,
                        std::int32_t index, const std::vector<Variant>& args)
{
    QPointer<QObject> guarded(object);
    return onObjectThread(object, [guarded, handshake, index, args] {
        if (!guarded) return Variant{};
        const QVariantList qargs = toQVariantList(args);
        if (handshake) {
            auto* proxy = qobject_cast<ModuleHandshakeProxy*>(guarded.data());
            if (!proxy || index != 0 || qargs.size() != 3) return Variant{};
            return fromQVariant(proxy->informModuleToken(qargs[0].toString(),
                                                         qargs[1].toString(),
                                                         qargs[2].toString()));
        }
        auto* proxy = qobject_cast<ModuleProxy*>(guarded.data());
        if (!proxy) return Variant{};
        switch (index) {
        case 0:
        case 1:
            if (qargs.size() < 2) return Variant{};
            return fromQVariant(proxy->callRemoteMethod(
                qargs[0].toString(), qargs[1].toString(),
                qargs.size() >= 3 ? qargs[2].toList() : QVariantList{}));
        case 2:
            if (qargs.size() != 4) return Variant{};
            return fromQVariant(proxy->callRemoteMethod(
                qargs[0].toString(), qargs[1].toString(), qargs[2].toList(),
                qargs[3].toString()));
        case 3:
            if (qargs.size() != 3) return Variant{};
            return fromQVariant(proxy->informModuleToken(
                qargs[0].toString(), qargs[1].toString(), qargs[2].toString()));
        case 4: return fromQVariant(proxy->getPluginMethods());
        case 5: return fromQVariant(proxy->getPluginEvents());
        case 6: return fromQVariant(proxy->getPluginInterface());
        default: return Variant{};
        }
    });
}

struct HandleState {
    std::mutex mutex;
    std::condition_variable completionChanged;
    std::map<QString, std::vector<LogosObject::EventCallback>> callbacks;
    std::map<QString, QVariant> completions;
    bool active = true;
};

} // namespace

QtRemotePlainTransportHost::QtRemotePlainTransportHost(const QString& url)
{
    std::string error;
    if (!m_server.start(url.toStdString(), &error))
        qCritical() << "QtRemotePlainTransportHost:" << error.c_str();
}

QtRemotePlainTransportHost::~QtRemotePlainTransportHost()
{
    for (const auto& entry : m_eventConnections) QObject::disconnect(entry.second);
}

bool QtRemotePlainTransportHost::publishObject(const QString& name, QObject* object)
{
    if (!object || !m_server.isRunning()) return false;
    const bool handshake = qobject_cast<ModuleHandshakeProxy*>(object) != nullptr;
    if (!handshake && !qobject_cast<ModuleProxy*>(object)) return false;
    const std::string key = name.toStdString();
    Server::Object published{
        key,
        handshake ? moduleHandshakeProxyDefinition() : moduleProxyDefinition(),
        [guarded = QPointer<QObject>(object), handshake](std::int32_t index,
                                                        const std::vector<Variant>& args) {
            return invokePublished(guarded.data(), handshake, index, args);
        }};
    std::string error;
    if (!m_server.publish(std::move(published), &error)) {
        qWarning() << "QtRemotePlainTransportHost: publish failed:" << error.c_str();
        return false;
    }
    if (!handshake) {
        auto* proxy = qobject_cast<ModuleProxy*>(object);
        m_eventConnections[key] = QObject::connect(
            proxy, &ModuleProxy::eventResponse, proxy,
            [this, key](const QString& eventName, const QVariantList& data) {
                std::vector<Variant> wireData;
                wireData.reserve(static_cast<std::size_t>(data.size()));
                for (const QVariant& item : data) wireData.push_back(fromQVariant(item));
                Variant list;
                list.type = MetaType::VariantList;
                list.isNull = false;
                list.value = plain::qvariantToRpcValue(data);
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
    if (auto it = m_eventConnections.find(key); it != m_eventConnections.end()) {
        QObject::disconnect(it->second);
        m_eventConnections.erase(it);
    }
    m_server.unpublish(key);
}

struct QtRemotePlainTransportConnection::Shared {
    std::shared_ptr<Client> client = std::make_shared<Client>();
    std::mutex mutex;
    std::unordered_map<std::string, std::vector<std::weak_ptr<HandleState>>> handles;
};

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
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                active = state->active;
            }
            if (active && callback) callback(std::move(value), error);
        }).detach();
    }

    bool informModuleToken(const QString& authToken, const QString& moduleName,
                           const QString& token, int timeoutMs) override
    {
        std::string error;
        auto result = m_shared->client->call(m_object,
            "informModuleToken(QString,QString,QString)",
            {fromQVariant(authToken), fromQVariant(moduleName), fromQVariant(token)},
            timeoutFor(timeoutMs), &error);
        return result && toQVariant(*result).toBool();
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
        std::string error;
        auto result = m_shared->client->call(m_object, "getPluginMethods()", {},
                                             timeoutFor(kDefaultTimeoutMs), &error);
        return result ? toQVariant(*result).toJsonArray() : QJsonArray{};
    }

    void release() override
    {
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
    std::weak_ptr<Shared> weak = m_shared;
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
                    if (logos::isReservedEventName(eventName) && data.size() == 2) {
                        state->completions[data[0].toString()] = data[1];
                        state->completionChanged.notify_all();
                        continue;
                    }
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

bool QtRemotePlainTransportConnection::connectToHost()
{
    if (m_shared->client->isConnected()) return true;
    std::string error;
    const bool connected = m_shared->client->connect(m_url, std::chrono::seconds(5), &error);
    if (!connected) qWarning() << "QtRemotePlainTransportConnection:" << error.c_str();
    return connected;
}

bool QtRemotePlainTransportConnection::isConnected() const
{
    return m_shared->client->isConnected();
}

bool QtRemotePlainTransportConnection::reconnect()
{
    m_shared->client->close();
    return connectToHost();
}

LogosObject* QtRemotePlainTransportConnection::requestObject(const QString& objectName,
                                                             int timeoutMs)
{
    if (!connectToHost()) return nullptr;
    std::string error;
    const std::string object = objectName.toStdString();
    if (!m_shared->client->acquire(object, timeoutFor(timeoutMs), &error)) return nullptr;
    auto state = std::make_shared<HandleState>();
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->handles[object].push_back(state);
    }
    return new QtRemotePlainLogosObject(m_shared, object, std::move(state));
}

} // namespace logos::qt_remote_plain
