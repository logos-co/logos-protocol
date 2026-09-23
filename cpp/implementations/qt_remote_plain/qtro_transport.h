#ifndef LOGOS_QT_REMOTE_PLAIN_QTRO_TRANSPORT_H
#define LOGOS_QT_REMOTE_PLAIN_QTRO_TRANSPORT_H

#include "qtro_wire.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace logos::qt_remote_plain {

// Resolves a local: endpoint without Qt. Unix uses the same absolute/temp-path
// convention as QLocalSocket. Windows plain peers use a byte-mode named pipe;
// Qt interoperability is intentionally required only on Unix platforms.
std::string localSocketPath(std::string_view localUrlOrPath);

class Client {
public:
    using EventHandler = std::function<void(
        const std::string& object,
        std::int32_t signalIndex,
        std::vector<Variant> arguments)>;
    using InternalEventHandler = std::function<bool(
        const std::string& object,
        std::int32_t signalIndex,
        const std::vector<Variant>& arguments)>;
    using DisconnectHandler = std::function<void(const std::string& reason)>;

    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect(const std::string& localUrlOrPath,
                 std::chrono::milliseconds timeout,
                 std::string* error = nullptr);
    void close();
    // Stop I/O without waiting for public event callbacks. Use when the caller
    // itself holds a callback lock; the executor is drained on final teardown.
    void closeWithoutWaitingForCallbacks();
    bool isConnected() const;

    bool acquire(const std::string& object,
                 std::chrono::milliseconds timeout,
                 std::string* error = nullptr);
    std::optional<ClassDefinition> definition(const std::string& object) const;

    std::optional<Variant> call(
        const std::string& object,
        const std::string& methodSignature,
        std::vector<Variant> arguments,
        std::chrono::milliseconds timeout,
        std::string* error = nullptr);

    void setEventHandler(EventHandler handler);
    // Runs on the reader before public callback dispatch. Returning true
    // consumes the signal. Reserved for transport control events whose state
    // must advance even while a user callback is blocked.
    void setInternalEventHandler(InternalEventHandler handler);
    void setDisconnectHandler(DisconnectHandler handler);

private:
    void closeConnection(bool waitForCallbacks);
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

class Server {
public:
    using InvokeHandler = std::function<Variant(
        std::int32_t methodIndex,
        const std::vector<Variant>& arguments)>;

    struct Object {
        std::string name;
        ClassDefinition definition;
        InvokeHandler invoke;
    };

    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const std::string& localUrlOrPath, std::string* error = nullptr);
    void stop();
    bool isRunning() const;

    bool publish(Object object, std::string* error = nullptr);
    void unpublish(const std::string& name);
    // Queues the event for every listener and returns without waiting on any.
    bool emitSignal(const std::string& object,
                    std::int32_t signalIndex,
                    const std::vector<Variant>& arguments,
                    std::string* error = nullptr);

    // For connections accepted afterwards: a peer whose unsent frames exceed
    // `maxQueuedBytes`, or that reads nothing for `stallTimeout` while frames
    // wait, is disconnected. Defaults: 64 MiB, 30 s.
    void setWriteLimits(std::size_t maxQueuedBytes, std::chrono::milliseconds stallTimeout);

    std::string socketPath() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace logos::qt_remote_plain

#endif
