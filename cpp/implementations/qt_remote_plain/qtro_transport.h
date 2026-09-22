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
    using DisconnectHandler = std::function<void(const std::string& reason)>;

    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect(const std::string& localUrlOrPath,
                 std::chrono::milliseconds timeout,
                 std::string* error = nullptr);
    void close();
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
    void setDisconnectHandler(DisconnectHandler handler);

private:
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
    bool emitSignal(const std::string& object,
                    std::int32_t signalIndex,
                    const std::vector<Variant>& arguments,
                    std::string* error = nullptr);

    std::string socketPath() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace logos::qt_remote_plain

#endif
