#include "implementations/plain_local/inproc_transport.h"

#include "logos_reserved_events.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace logos::plain::inproc {

namespace {

constexpr std::size_t kDefaultWorkers = 64;
constexpr std::chrono::seconds kWorkerIdle{5};

ResultMessage closedResult(std::uint64_t id, const std::string& reason)
{
    ResultMessage result;
    result.id = id;
    result.err = reason;
    result.errCode = "TRANSPORT_CLOSED";
    return result;
}

} // namespace

bool isControlMethod(const std::string& method)
{
    return method == "informModuleToken" || method == "revokeModuleToken"
        || method == "getPluginMethods" || method == "getPluginEvents"
        || method == "getPluginInterface";
}

class Connection;

class Endpoint : public std::enable_shared_from_this<Endpoint> {
public:
    Endpoint(std::string registryKey, std::string objectName, Handlers callbacks,
             std::size_t workerLimit)
        : key(std::move(registryKey)), object(std::move(objectName)),
          handlers(std::move(callbacks)),
          m_maxWorkers(workerLimit ? workerLimit : kDefaultWorkers) {}

    const std::string key;
    const std::string object;
    const Handlers handlers;

    bool open() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_stopped;
    }

    void setMaxWorkers(std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maxWorkers = limit ? limit : kDefaultWorkers;
    }

    // False once withdrawn: the job will never run.
    bool submit(std::function<void()> job)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) return false;
        m_calls.push_back(std::move(job));
        if (m_idle < m_calls.size() && m_workers < m_maxWorkers) {
            auto self = shared_from_this();
            try {
                std::thread([self] { self->workerLoop(); }).detach();
                ++m_workers;
            } catch (...) {
                if (m_workers == 0) {
                    m_calls.pop_back();
                    return false;
                }
            }
        }
        m_changed.notify_all();
        return true;
    }

    bool submitControl(std::function<void()> job)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) return false;
        m_controls.push_back(std::move(job));
        if (!m_controlWorker) {
            auto self = shared_from_this();
            try {
                std::thread([self] { self->controlLoop(); }).detach();
                m_controlWorker = true;
            } catch (...) {
                m_controls.pop_back();
                return false;
            }
        }
        m_changed.notify_all();
        return true;
    }

    bool attach(const std::shared_ptr<Connection>& connection)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) return false;
        m_connections[connection.get()] = connection;
        return true;
    }

    void detach(const Connection* connection)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.erase(connection);
        m_sinks.erase(connection);
    }

    void subscribe(const Connection* connection, const std::string& event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopped && m_connections.count(connection)) m_sinks[connection].insert(event);
    }

    void unsubscribe(const Connection* connection, const std::string& event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_sinks.find(connection);
        if (found == m_sinks.end()) return;
        found->second.erase(event);
        if (found->second.empty()) m_sinks.erase(found);
    }

    void emit(const std::string& objectName, const std::string& event,
              const std::vector<RpcValue>& data);

    void withdraw();

private:
    void workerLoop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            ++m_idle;
            m_changed.wait_for(lock, kWorkerIdle, [&] { return m_stopped || !m_calls.empty(); });
            --m_idle;
            if (m_calls.empty()) break;
            std::function<void()> job = std::move(m_calls.front());
            m_calls.pop_front();
            ++m_running;
            m_busy.insert(std::this_thread::get_id());
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
            m_busy.erase(m_busy.find(std::this_thread::get_id()));
            --m_running;
            m_changed.notify_all();
        }
        --m_workers;
        m_changed.notify_all();
    }

    void controlLoop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_changed.wait_for(lock, kWorkerIdle, [&] { return m_stopped || !m_controls.empty(); });
            if (m_controls.empty()) break;
            std::function<void()> job = std::move(m_controls.front());
            m_controls.pop_front();
            ++m_running;
            m_busy.insert(std::this_thread::get_id());
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
            m_busy.erase(m_busy.find(std::this_thread::get_id()));
            --m_running;
            m_changed.notify_all();
        }
        m_controlWorker = false;
        m_changed.notify_all();
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_stopped = false;
    std::size_t m_maxWorkers;
    std::deque<std::function<void()>> m_calls;
    std::deque<std::function<void()>> m_controls;
    std::size_t m_workers = 0;
    std::size_t m_idle = 0;
    std::size_t m_running = 0;
    bool m_controlWorker = false;
    std::multiset<std::thread::id> m_busy;
    std::map<const Connection*, std::weak_ptr<Connection>> m_connections;
    // Per connection, the events it receives; "" is every non-reserved one.
    std::map<const Connection*, std::set<std::string>> m_sinks;
};

class Connection final : public RpcConnectionBase,
                         public std::enable_shared_from_this<Connection> {
public:
    Connection(std::shared_ptr<Endpoint> endpoint, std::string principal)
        : m_endpoint(std::move(endpoint)), m_principal(std::move(principal)) {}

    ~Connection() override { shutdown("connection destroyed", false); }

    void start() override {}
    void stop(const std::string& reason) override { shutdown(reason, false); }
    bool isOpen() const override { return !m_stopped.load(); }

    std::future<ResultMessage> sendCall(CallMessage message) override
    {
        auto promise = std::make_shared<std::promise<ResultMessage>>();
        auto future = promise->get_future();
        sendCallAsync(std::move(message), [promise](ResultMessage result) {
            promise->set_value(std::move(result));
        });
        return future;
    }

    void sendCallAsync(CallMessage message, ResultHandler handler) override
    {
        const std::uint64_t id = message.id;
        bool registered = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stopped) {
                m_pending[id] = std::move(handler);
                registered = true;
            }
        }
        if (!registered) {
            // Stopped before the call: answered here, as RpcConnection does.
            if (handler) handler(closedResult(id, "connection stopped"));
            return;
        }
        // A teardown that swept the registration has already answered it.
        if (m_stopped) return;
        const bool control = isControlMethod(message.method);
        std::weak_ptr<Connection> weak = shared_from_this();
        auto endpoint = m_endpoint;
        auto principal = m_principal;
        auto job = [weak, endpoint, principal, control, message = std::move(message)] {
            ResultMessage result;
            try {
                result = control ? endpoint->handlers.control(message, principal)
                                 : endpoint->handlers.call(message, principal);
            } catch (const std::exception& error) {
                result.err = error.what();
                result.errCode = "METHOD_FAILED";
            } catch (...) {
                result.err = "method failed";
                result.errCode = "METHOD_FAILED";
            }
            result.id = message.id;
            if (auto connection = weak.lock()) connection->complete(message.id, std::move(result));
        };
        const bool queued = control ? m_endpoint->submitControl(std::move(job))
                                    : m_endpoint->submit(std::move(job));
        if (!queued) shutdown("provider withdrawn", true);
    }

    std::future<MethodsResultMessage> sendMethods(MethodsMessage message) override
    {
        auto promise = std::make_shared<std::promise<MethodsResultMessage>>();
        auto future = promise->get_future();
        const std::uint64_t id = message.id;
        auto endpoint = m_endpoint;
        const bool queued = !m_stopped && m_endpoint->submitControl(
            [promise, endpoint, message = std::move(message)] {
                MethodsResultMessage result;
                try {
                    result = endpoint->handlers.methods(message);
                } catch (const std::exception& error) {
                    result.err = error.what();
                }
                result.id = message.id;
                promise->set_value(std::move(result));
            });
        if (!queued) {
            MethodsResultMessage failure;
            failure.id = id;
            failure.err = "connection stopped";
            promise->set_value(std::move(failure));
        }
        return future;
    }

    void cancelPending(std::uint64_t id) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(id);
    }

    SubscriptionId sendSubscribe(SubscribeMessage message,
                                 std::function<void(EventMessage)> callback) override
    {
        SubscriptionId id = 0;
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            id = ++m_nextSubscription;
            first = std::none_of(m_registrations.begin(), m_registrations.end(),
                [&](const auto& entry) {
                    return entry.second.object == message.object
                        && entry.second.event == message.eventName;
                });
            m_registrations[id] = {message.object, message.eventName, std::move(callback)};
        }
        if (first && message.object == m_endpoint->object)
            m_endpoint->subscribe(this, message.eventName);
        return id;
    }

    void sendUnsubscribe(SubscriptionId id) override
    {
        std::string object;
        std::string event;
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_registrations.find(id);
            if (found == m_registrations.end()) return;
            object = found->second.object;
            event = found->second.event;
            m_registrations.erase(found);
            last = std::none_of(m_registrations.begin(), m_registrations.end(),
                [&](const auto& entry) {
                    return entry.second.object == object && entry.second.event == event;
                });
        }
        if (last && object == m_endpoint->object) m_endpoint->unsubscribe(this, event);
    }

    void sendEvent(EventMessage) override {}

    // Applied before this returns, so a call sent after it sees the token, as a
    // later frame on one stream connection would.
    void sendToken(TokenMessage message) override
    {
        if (m_stopped) return;
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        auto endpoint = m_endpoint;
        auto principal = m_principal;
        if (!m_endpoint->submitControl([done, endpoint, principal, message = std::move(message)] {
                try {
                    (void)endpoint->handlers.token(message, principal);
                } catch (...) {}
                done->set_value();
            }))
            return;
        finished.wait();
    }

    void setErrorHandler(ErrorHandler handler) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_onError = std::move(handler);
    }

    std::uint64_t nextId() override { return ++m_nextId; }

    void complete(std::uint64_t id, ResultMessage result)
    {
        ResultHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_pending.find(id);
            if (found == m_pending.end()) return;
            handler = std::move(found->second);
            m_pending.erase(found);
        }
        handler(std::move(result));
    }

    void deliver(const EventMessage& message)
    {
        std::vector<std::function<void(EventMessage)>> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopped) return;
            const bool reserved = logos::isReservedEventName(message.eventName);
            for (const auto& entry : m_registrations) {
                if (entry.second.object != message.object) continue;
                if (entry.second.event == message.eventName
                    || (entry.second.event.empty() && !reserved))
                    callbacks.push_back(entry.second.callback);
            }
        }
        for (const auto& callback : callbacks) callback(message);
    }

    // The endpoint went away: every pending call fails and the owner is told.
    void lost(const std::string& reason) { shutdown(reason, true); }

private:
    struct Registration {
        std::string object;
        std::string event;
        std::function<void(EventMessage)> callback;
    };

    void shutdown(const std::string& reason, bool notify)
    {
        if (m_stopped.exchange(true)) return;
        std::map<std::uint64_t, ResultHandler> pending;
        ErrorHandler onError;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pending.swap(m_pending);
            onError = m_onError;
        }
        m_endpoint->detach(this);
        for (auto& [id, handler] : pending)
            if (handler) handler(closedResult(id, reason));
        if (notify && onError) onError(reason);
    }

    std::shared_ptr<Endpoint> m_endpoint;
    const std::string m_principal;
    std::atomic<bool> m_stopped{false};
    std::atomic<std::uint64_t> m_nextId{0};
    std::mutex m_mutex;
    std::map<std::uint64_t, ResultHandler> m_pending;
    ErrorHandler m_onError;
    std::map<SubscriptionId, Registration> m_registrations;
    SubscriptionId m_nextSubscription = 0;
};

void Endpoint::emit(const std::string& objectName, const std::string& event,
                    const std::vector<RpcValue>& data)
{
    std::vector<std::shared_ptr<Connection>> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped || objectName != object) return;
        const bool reserved = logos::isReservedEventName(event);
        for (const auto& [id, events] : m_sinks) {
            if (!events.count(event) && (reserved || !events.count(std::string{}))) continue;
            const auto found = m_connections.find(id);
            if (found == m_connections.end()) continue;
            if (auto connection = found->second.lock()) targets.push_back(std::move(connection));
        }
    }
    const EventMessage message{objectName, event, data};
    for (const auto& connection : targets) connection->deliver(message);
}

namespace {

std::mutex& registryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::weak_ptr<Endpoint>>& registry()
{
    static std::map<std::string, std::weak_ptr<Endpoint>> endpoints;
    return endpoints;
}

std::string registryKey(const std::string& instance, const std::string& module)
{
    return instance + '/' + module;
}

std::shared_ptr<Endpoint> lookup(const std::string& instance, const std::string& module)
{
    std::lock_guard<std::mutex> lock(registryMutex());
    const auto found = registry().find(registryKey(instance, module));
    if (found == registry().end()) return {};
    return found->second.lock();
}

} // namespace

void Endpoint::withdraw()
{
    std::vector<std::shared_ptr<Connection>> connections;
    {
        std::lock_guard<std::mutex> registryLock(registryMutex());
        const auto found = registry().find(key);
        if (found != registry().end()) {
            const auto current = found->second.lock();
            if (!current || current.get() == this) registry().erase(found);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) return;
        m_stopped = true;
        // Calls not yet started never will: their connections fail them below.
        m_calls.clear();
        m_controls.clear();
        for (const auto& entry : m_connections)
            if (auto connection = entry.second.lock()) connections.push_back(std::move(connection));
        m_changed.notify_all();
    }
    for (const auto& connection : connections) connection->lost("provider withdrawn");
    // The provider's callbacks are captured by running calls: they finish first.
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto self = std::this_thread::get_id();
    m_changed.wait(lock, [&] { return m_running == m_busy.count(self); });
}

std::shared_ptr<Endpoint> publish(const std::string& instance, const std::string& module,
                                  Handlers handlers, std::size_t maxWorkers)
{
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& slot = registry()[registryKey(instance, module)];
    if (auto existing = slot.lock(); existing && existing->open()) return {};
    auto endpoint = std::make_shared<Endpoint>(registryKey(instance, module), module,
                                               std::move(handlers), maxWorkers);
    slot = endpoint;
    return endpoint;
}

void withdraw(const std::shared_ptr<Endpoint>& endpoint)
{
    if (endpoint) endpoint->withdraw();
}

void setMaxWorkers(const std::shared_ptr<Endpoint>& endpoint, std::size_t maxWorkers)
{
    if (endpoint) endpoint->setMaxWorkers(maxWorkers);
}

bool isPublished(const std::string& instance, const std::string& module)
{
    const auto endpoint = lookup(instance, module);
    return endpoint && endpoint->open();
}

void emit(const std::shared_ptr<Endpoint>& endpoint, const std::string& object,
          const std::string& event, const std::vector<RpcValue>& data)
{
    if (endpoint) endpoint->emit(object, event, data);
}

std::shared_ptr<RpcConnectionBase> connect(const std::string& instance,
                                           const std::string& module,
                                           const std::string& principal,
                                           std::string& error)
{
    auto endpoint = lookup(instance, module);
    if (!endpoint) {
        error = "object not published: " + module;
        return {};
    }
    auto connection = std::make_shared<Connection>(endpoint, principal);
    if (!endpoint->attach(connection)) {
        error = "object not published: " + module;
        return {};
    }
    return connection;
}

} // namespace logos::plain::inproc
