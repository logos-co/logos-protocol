#include "qtro_transport.h"

#include "../../logos_socket_paths.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace logos::qt_remote_plain {
namespace {

constexpr std::size_t kMaximumFrame = 64u << 20;
using NativeHandle = std::intptr_t;
constexpr NativeHandle kInvalidHandle = -1;

void setError(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

#ifdef _WIN32

HANDLE winHandle(NativeHandle handle)
{
    return reinterpret_cast<HANDLE>(handle);
}

std::string windowsError(const char* operation)
{
    return std::string(operation) + " failed (Windows error "
        + std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
}

std::string windowsError(const char* operation, DWORD code)
{
    return std::string(operation) + " failed (Windows error "
        + std::to_string(static_cast<unsigned long>(code)) + ")";
}

bool overlappedWrite(HANDLE handle, const std::uint8_t* data, DWORD size,
                     DWORD& written, std::string* error)
{
    OVERLAPPED operation{};
    operation.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) {
        setError(error, windowsError("CreateEvent"));
        return false;
    }
    bool success = ::WriteFile(handle, data, size, &written, &operation) != FALSE;
    if (!success) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_IO_PENDING) {
            success = ::WaitForSingleObject(operation.hEvent, INFINITE) == WAIT_OBJECT_0
                && ::GetOverlappedResult(handle, &operation, &written, FALSE) != FALSE;
            if (!success)
                setError(error, windowsError("WriteFile", ::GetLastError()));
        } else {
            setError(error, windowsError("WriteFile", code));
        }
    }
    ::CloseHandle(operation.hEvent);
    return success;
}

bool overlappedRead(HANDLE handle, std::uint8_t* data, DWORD size, DWORD& received)
{
    OVERLAPPED operation{};
    operation.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    bool success = ::ReadFile(handle, data, size, &received, &operation) != FALSE;
    if (!success) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_IO_PENDING) {
            success = ::WaitForSingleObject(operation.hEvent, INFINITE) == WAIT_OBJECT_0
                && ::GetOverlappedResult(handle, &operation, &received, FALSE) != FALSE;
        }
    }
    ::CloseHandle(operation.hEvent);
    return success;
}

bool writeAll(NativeHandle handle, const std::vector<std::uint8_t>& data,
              std::string* error)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(
            data.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!overlappedWrite(winHandle(handle), data.data() + offset, remaining,
                             written, error) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool readExact(NativeHandle handle, std::uint8_t* output, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        DWORD received = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, std::numeric_limits<DWORD>::max()));
        if (!overlappedRead(winHandle(handle), output + offset, remaining, received)
            || received == 0)
            return false;
        offset += received;
    }
    return true;
}

#else

bool writeAll(NativeHandle fd, const std::vector<std::uint8_t>& data, std::string* error)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto count = ::send(fd, data.data() + offset, data.size() - offset, flags);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            setError(error, "local socket write failed: " + std::string(std::strerror(errno)));
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool readExact(NativeHandle fd, std::uint8_t* output, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::recv(fd, output + offset, size - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

#endif

bool readPacket(NativeHandle handle, std::vector<std::uint8_t>& output, std::string* error)
{
    output.resize(4);
    if (!readExact(handle, output.data(), 4)) {
        setError(error, "local socket closed");
        return false;
    }
    const std::uint32_t payload = static_cast<std::uint32_t>(output[0])
        | (static_cast<std::uint32_t>(output[1]) << 8)
        | (static_cast<std::uint32_t>(output[2]) << 16)
        | (static_cast<std::uint32_t>(output[3]) << 24);
    if (payload > kMaximumFrame) {
        setError(error, "QtRO frame exceeds transport limit");
        return false;
    }
    output.resize(4 + payload);
    if (!readExact(handle, output.data() + 4, payload)) {
        setError(error, "local socket closed in the middle of a frame");
        return false;
    }
    return true;
}

void closeHandle(std::atomic<NativeHandle>& handle)
{
    const NativeHandle value = handle.exchange(kInvalidHandle);
    if (value == kInvalidHandle) return;
#ifdef _WIN32
    ::CancelIoEx(winHandle(value), nullptr);
    ::DisconnectNamedPipe(winHandle(value));
    ::CloseHandle(winHandle(value));
#else
    {
        ::shutdown(value, SHUT_RDWR);
        ::close(value);
    }
#endif
}

#ifdef _WIN32
NativeHandle connectSocket(const std::string& path,
                           std::chrono::milliseconds timeout,
                           std::string* error)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        HANDLE pipe = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            return reinterpret_cast<NativeHandle>(pipe);

        const DWORD code = ::GetLastError();
        if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) {
            setError(error, windowsError("CreateFile"));
            return kInvalidHandle;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            setError(error, "named pipe connection timed out: " + path);
            return kInvalidHandle;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const DWORD wait = static_cast<DWORD>(std::min<std::int64_t>(
            std::max<std::int64_t>(1, remaining.count()), 50));
        if (code == ERROR_PIPE_BUSY) (void)::WaitNamedPipeA(path.c_str(), wait);
        else ::Sleep(wait);
    }
}
#else
NativeHandle connectSocket(const std::string& path,
                           std::chrono::milliseconds,
                           std::string* error)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        setError(error, "socket() failed: " + std::string(std::strerror(errno)));
        return kInvalidHandle;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        setError(error, "local socket path is too long: " + path);
        ::close(fd);
        return kInvalidHandle;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        setError(error, "connect(" + path + ") failed: " + std::string(std::strerror(errno)));
        ::close(fd);
        return kInvalidHandle;
    }
    return fd;
}

#endif

} // namespace

std::string localSocketPath(std::string_view localUrlOrPath)
{
    constexpr std::string_view prefix = "local:";
    if (localUrlOrPath.substr(0, prefix.size()) == prefix)
        localUrlOrPath.remove_prefix(prefix.size());
    if (localUrlOrPath.empty()) return {};
#ifdef _WIN32
    constexpr std::string_view pipePrefix = R"(\\.\pipe\)";
    if (localUrlOrPath.substr(0, pipePrefix.size()) == pipePrefix)
        return std::string(localUrlOrPath);
    std::string name(localUrlOrPath);
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), ':', '_');
    return std::string(pipePrefix) + name;
#else
    if (localUrlOrPath.front() == '/') return std::string(localUrlOrPath);
    const char* configured = std::getenv("TMPDIR");
    std::string temp = configured && *configured ? configured : "/tmp";
    while (temp.size() > 1 && temp.back() == '/') temp.pop_back();
    return temp + "/" + std::string(localUrlOrPath);
#endif
}

struct Client::Impl : std::enable_shared_from_this<Client::Impl> {
    struct Pending {
        std::condition_variable cv;
        bool done = false;
        Variant value;
        std::string error;
    };

    mutable std::mutex mu;
    std::mutex sendMu;
    std::condition_variable changed;
    std::atomic<NativeHandle> fd{kInvalidHandle};
    std::atomic<bool> running{false};
    bool readerExited = true;
    bool handshake = false;
    bool objectListSeen = false;
    std::string failure;
    std::map<std::string, ObjectInfo> advertised;
    std::map<std::string, ClassDefinition> definitions;
    std::set<std::string> requested;
    std::map<std::int32_t, std::shared_ptr<Pending>> pending;
    std::int32_t nextSerial = 1;
    EventHandler eventHandler;
    DisconnectHandler disconnectHandler;
    bool disconnectReported = false;
    std::thread::id readerThread;

    bool send(const std::vector<std::uint8_t>& frame, std::string* error = nullptr)
    {
        std::lock_guard<std::mutex> lock(sendMu);
        const NativeHandle current = fd.load();
        if (current == kInvalidHandle) {
            setError(error, "local socket is closed");
            return false;
        }
        return writeAll(current, frame, error);
    }

    void fail(std::string reason)
    {
        std::map<std::int32_t, std::shared_ptr<Pending>> calls;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!failure.empty()) return;
            failure = std::move(reason);
            running = false;
            calls = pending;
            pending.clear();
            for (auto& [unused, call] : calls) {
                (void)unused;
                call->done = true;
                call->error = failure;
            }
        }
        for (auto& [unused, call] : calls) {
            (void)unused;
            call->cv.notify_all();
        }
        changed.notify_all();
    }

    void handle(Frame frame)
    {
        switch (frame.type) {
        case PacketType::Handshake: {
            std::lock_guard<std::mutex> lock(mu);
            if (frame.name != "QtRO 2.0") {
                failure = "QtRO protocol mismatch: " + frame.name;
                running = false;
            } else {
                handshake = true;
            }
            changed.notify_all();
            break;
        }
        case PacketType::ObjectList: {
            Reader reader(frame.payload);
            const auto count = reader.u32();
            std::map<std::string, ObjectInfo> update;
            for (std::uint32_t i = 0; i < count; ++i) {
                ObjectInfo info;
                info.name = reader.string().value_or(std::string{});
                info.typeName = reader.string();
                info.signature = reader.bytes().value_or(std::vector<std::uint8_t>{});
                update[info.name] = std::move(info);
            }
            {
                std::lock_guard<std::mutex> lock(mu);
                advertised.insert(update.begin(), update.end());
                objectListSeen = true;
            }
            changed.notify_all();
            break;
        }
        case PacketType::InitDynamic: {
            Reader reader(frame.payload);
            auto definition = reader.classDefinition();
            const auto properties = reader.u32();
            for (std::uint32_t i = 0; i < properties; ++i) (void)reader.variant();
            {
                std::lock_guard<std::mutex> lock(mu);
                definitions[frame.name] = std::move(definition);
            }
            changed.notify_all();
            break;
        }
        case PacketType::RemoveObject: {
            std::lock_guard<std::mutex> lock(mu);
            advertised.erase(frame.name);
            definitions.erase(frame.name);
            requested.erase(frame.name);
            changed.notify_all();
            break;
        }
        case PacketType::InvokeReply: {
            Reader reader(frame.payload);
            const auto serial = reader.i32();
            Variant value = reader.variant();
            std::shared_ptr<Pending> call;
            {
                std::lock_guard<std::mutex> lock(mu);
                const auto it = pending.find(serial);
                if (it == pending.end()) break;
                call = it->second;
                pending.erase(it);
                call->value = std::move(value);
                call->done = true;
            }
            call->cv.notify_all();
            break;
        }
        case PacketType::Invoke: {
            Reader reader(frame.payload);
            const auto callKind = reader.i32();
            const auto index = reader.i32();
            const auto count = reader.u32();
            std::vector<Variant> args;
            args.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) args.push_back(reader.variant());
            (void)reader.i32();
            (void)reader.i32();
            EventHandler callback;
            {
                std::lock_guard<std::mutex> lock(mu);
                callback = eventHandler;
            }
            if (callKind == 0 && callback)
                callback(frame.name, index, std::move(args));
            break;
        }
        case PacketType::Ping:
            (void)send(pingPacket(PacketType::Pong, frame.name));
            break;
        default:
            break;
        }
    }

    void readLoop()
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            readerThread = std::this_thread::get_id();
        }
        std::string error;
        std::vector<std::uint8_t> bytes;
        while (running && readPacket(fd.load(), bytes, &error)) {
            try {
                handle(decodeFrame(bytes));
            } catch (const std::exception& exception) {
                error = exception.what();
                break;
            }
        }
        if (error.empty()) error = "local socket closed";
        fail(error);
        closeHandle(fd);
        DisconnectHandler disconnected;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!disconnectReported) {
                disconnectReported = true;
                disconnected = disconnectHandler;
            }
        }
        // Report after the old descriptor is closed, but before close()/connect()
        // observes readerExited.  A reconnect requested by the callback can then
        // wait for this reader to finish without racing it against the new fd.
        if (disconnected) disconnected(error);
        {
            std::lock_guard<std::mutex> lock(mu);
            readerExited = true;
        }
        changed.notify_all();
    }
};

Client::Client() : m_impl(std::make_shared<Impl>()) {}
Client::~Client() { close(); }

bool Client::connect(const std::string& localUrlOrPath,
                     std::chrono::milliseconds timeout,
                     std::string* error)
{
    close();
    const std::string path = localSocketPath(localUrlOrPath);
    const NativeHandle socket = connectSocket(path, timeout, error);
    if (socket == kInvalidHandle) return false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->fd = socket;
        m_impl->running = true;
        m_impl->readerExited = false;
        m_impl->handshake = false;
        m_impl->objectListSeen = false;
        m_impl->failure.clear();
        m_impl->advertised.clear();
        m_impl->definitions.clear();
        m_impl->requested.clear();
        m_impl->disconnectReported = false;
    }
    auto state = m_impl;
    std::thread([state] { state->readLoop(); }).detach();

    std::unique_lock<std::mutex> lock(m_impl->mu);
    const bool ready = m_impl->changed.wait_for(lock, timeout, [&] {
        return (m_impl->handshake && m_impl->objectListSeen) || !m_impl->failure.empty();
    });
    if (!ready || !m_impl->handshake || !m_impl->objectListSeen) {
        const std::string why = !m_impl->failure.empty() ? m_impl->failure : "QtRO handshake timed out";
        lock.unlock();
        close();
        setError(error, why);
        return false;
    }
    return true;
}

void Client::close()
{
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (m_impl->readerExited && m_impl->fd.load() == kInvalidHandle) return;
        m_impl->running = false;
    }
    closeHandle(m_impl->fd);
    std::unique_lock<std::mutex> lock(m_impl->mu);
    if (m_impl->readerThread == std::this_thread::get_id()) return;
    m_impl->changed.wait_for(lock, std::chrono::seconds(2), [&] { return m_impl->readerExited; });
}

bool Client::isConnected() const
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->running && m_impl->handshake && m_impl->failure.empty();
}

bool Client::acquire(const std::string& object,
                     std::chrono::milliseconds timeout,
                     std::string* error)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    {
        std::unique_lock<std::mutex> lock(m_impl->mu);
        if (!m_impl->changed.wait_until(lock, deadline, [&] {
                return m_impl->advertised.count(object) != 0 || !m_impl->failure.empty();
            })) {
            setError(error, "object was not advertised before the acquire deadline: " + object);
            return false;
        }
        if (!m_impl->failure.empty()) {
            setError(error, m_impl->failure);
            return false;
        }
        if (m_impl->definitions.count(object) != 0) return true;
        if (m_impl->requested.insert(object).second) {
            lock.unlock();
            std::string writeError;
            if (!m_impl->send(addObjectPacket(object), &writeError)) {
                setError(error, writeError);
                return false;
            }
            lock.lock();
        }
        if (!m_impl->changed.wait_until(lock, deadline, [&] {
                return m_impl->definitions.count(object) != 0 || !m_impl->failure.empty();
            })) {
            setError(error, "dynamic object definition timed out: " + object);
            return false;
        }
        if (!m_impl->failure.empty()) {
            setError(error, m_impl->failure);
            return false;
        }
    }
    return true;
}

std::optional<ClassDefinition> Client::definition(const std::string& object) const
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    const auto it = m_impl->definitions.find(object);
    if (it == m_impl->definitions.end()) return std::nullopt;
    return it->second;
}

std::optional<Variant> Client::call(
    const std::string& object,
    const std::string& methodSignature,
    std::vector<Variant> arguments,
    std::chrono::milliseconds timeout,
    std::string* error)
{
    if (!acquire(object, timeout, error)) return std::nullopt;

    std::int32_t methodIndex = -1;
    std::int32_t serial = -1;
    auto pending = std::make_shared<Impl::Pending>();
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        const auto& methods = m_impl->definitions.at(object).methodDefinitions;
        const auto it = std::find_if(methods.begin(), methods.end(), [&](const auto& method) {
            return method.signature == methodSignature;
        });
        if (it == methods.end()) {
            setError(error, "method is absent from dynamic object: " + methodSignature);
            return std::nullopt;
        }
        methodIndex = static_cast<std::int32_t>(std::distance(methods.begin(), it));
        serial = m_impl->nextSerial == std::numeric_limits<std::int32_t>::max()
            ? 1 : m_impl->nextSerial++;
        m_impl->pending[serial] = pending;
    }

    std::string writeError;
    if (!m_impl->send(invokePacket(object, 0, methodIndex, arguments, serial), &writeError)) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->pending.erase(serial);
        setError(error, writeError);
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(m_impl->mu);
    if (!pending->cv.wait_for(lock, timeout, [&] { return pending->done; })) {
        m_impl->pending.erase(serial);
        setError(error, "QtRO invocation timed out: " + object + "." + methodSignature);
        return std::nullopt;
    }
    if (!pending->error.empty()) {
        setError(error, pending->error);
        return std::nullopt;
    }
    return pending->value;
}

void Client::setEventHandler(EventHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->eventHandler = std::move(handler);
}

void Client::setDisconnectHandler(DisconnectHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->disconnectHandler = std::move(handler);
}

struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
    struct Connection {
        explicit Connection(NativeHandle value) : fd(value) {}
        std::atomic<NativeHandle> fd;
        std::mutex sendMu;
        std::set<std::string> acquired;
    };

    mutable std::mutex mu;
    std::condition_variable changed;
    std::atomic<NativeHandle> listener{kInvalidHandle};
    std::atomic<bool> running{false};
    bool acceptExited = true;
    std::string path;
    std::map<std::string, Object> objects;
    std::vector<std::shared_ptr<Connection>> connections;

    bool send(const std::shared_ptr<Connection>& connection,
              const std::vector<std::uint8_t>& frame,
              std::string* error = nullptr)
    {
        std::lock_guard<std::mutex> lock(connection->sendMu);
        const NativeHandle current = connection->fd.load();
        return current != kInvalidHandle && writeAll(current, frame, error);
    }

    std::vector<ObjectInfo> objectList() const
    {
        std::vector<ObjectInfo> result;
        result.reserve(objects.size());
        for (const auto& [name, object] : objects)
            result.push_back({name, std::nullopt, {}});
        return result;
    }

    void connectionLoop(const std::shared_ptr<Connection>& connection)
    {
        std::vector<std::uint8_t> bytes;
        std::string error;
        while (running && readPacket(connection->fd.load(), bytes, &error)) {
            try {
                const Frame frame = decodeFrame(bytes);
                if (frame.type == PacketType::Ping) {
                    (void)send(connection, pingPacket(PacketType::Pong, frame.name));
                    continue;
                }
                if (frame.type == PacketType::RemoveObject) {
                    std::lock_guard<std::mutex> lock(mu);
                    connection->acquired.erase(frame.name);
                    continue;
                }
                if (frame.type == PacketType::AddObject) {
                    Reader payload(frame.payload);
                    const bool dynamic = payload.boolean();
                    Object object;
                    bool found = false;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        const auto it = objects.find(frame.name);
                        if (it != objects.end()) {
                            object = it->second;
                            connection->acquired.insert(frame.name);
                            found = true;
                        }
                    }
                    if (found && dynamic)
                        (void)send(connection, initDynamicPacket(frame.name, object.definition));
                    continue;
                }
                if (frame.type == PacketType::Invoke) {
                    Reader payload(frame.payload);
                    const auto callKind = payload.i32();
                    const auto methodIndex = payload.i32();
                    const auto count = payload.u32();
                    std::vector<Variant> arguments;
                    arguments.reserve(count);
                    for (std::uint32_t i = 0; i < count; ++i)
                        arguments.push_back(payload.variant());
                    const auto serial = payload.i32();
                    (void)payload.i32();
                    Object object;
                    bool found = false;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        const auto it = objects.find(frame.name);
                        if (it != objects.end()) {
                            object = it->second;
                            found = true;
                        }
                    }
                    if (found && callKind == 0 && object.invoke) {
                        Variant result;
                        try {
                            result = object.invoke(methodIndex, arguments);
                        } catch (...) {
                            result = Variant::fromRpc(plain::RpcValue{});
                        }
                        if (serial >= 0)
                            (void)send(connection, invokeReplyPacket(frame.name, serial, result));
                    }
                }
            } catch (...) {
                break;
            }
        }
        closeHandle(connection->fd);
        {
            std::lock_guard<std::mutex> lock(mu);
            connections.erase(std::remove(connections.begin(), connections.end(), connection),
                              connections.end());
        }
        changed.notify_all();
    }

    void acceptLoop()
    {
        while (running) {
#ifdef _WIN32
            HANDLE pipe = ::CreateNamedPipeA(
                path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 64u << 10, 64u << 10, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) break;
            const NativeHandle candidate = reinterpret_cast<NativeHandle>(pipe);
            listener = candidate;
            OVERLAPPED operation{};
            operation.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
            bool connected = false;
            if (operation.hEvent) {
                connected = ::ConnectNamedPipe(pipe, &operation) != FALSE;
                if (!connected) {
                    const DWORD code = ::GetLastError();
                    if (code == ERROR_PIPE_CONNECTED) {
                        connected = true;
                    } else if (code == ERROR_IO_PENDING) {
                        DWORD transferred = 0;
                        connected = ::WaitForSingleObject(operation.hEvent, INFINITE)
                                == WAIT_OBJECT_0
                            && ::GetOverlappedResult(pipe, &operation,
                                                     &transferred, FALSE) != FALSE;
                    }
                }
                ::CloseHandle(operation.hEvent);
            }
            const NativeHandle owned = listener.exchange(kInvalidHandle);
            if (owned == kInvalidHandle) break;
            if (!connected) {
                std::atomic<NativeHandle> failed{owned};
                closeHandle(failed);
                if (running) continue;
                break;
            }
            const NativeHandle accepted = owned;
#else
            const NativeHandle accepted = ::accept(listener.load(), nullptr, nullptr);
            if (accepted < 0) {
                if (errno == EINTR) continue;
                break;
            }
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
            auto connection = std::make_shared<Connection>(accepted);
            std::vector<ObjectInfo> currentObjects;
            {
                std::lock_guard<std::mutex> lock(mu);
                if (!running) {
                    closeHandle(connection->fd);
                    break;
                }
                connections.push_back(connection);
                currentObjects = objectList();
            }
            if (!send(connection, handshakePacket())
                || !send(connection, objectListPacket(currentObjects))) {
                closeHandle(connection->fd);
            }
            auto state = shared_from_this();
            std::thread([state, connection] { state->connectionLoop(connection); }).detach();
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            acceptExited = true;
        }
        changed.notify_all();
    }
};

Server::Server() : m_impl(std::make_shared<Impl>()) {}
Server::~Server() { stop(); }

bool Server::start(const std::string& localUrlOrPath, std::string* error)
{
    stop();
    const std::string path = localSocketPath(localUrlOrPath);
    if (path.empty()) {
        setError(error, "empty local endpoint path");
        return false;
    }
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->path = path;
        m_impl->running = true;
        m_impl->acceptExited = false;
    }
#else
    if (logos::isSocketDead(path)) ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        setError(error, "socket() failed: " + std::string(std::strerror(errno)));
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        setError(error, "local socket path is too long: " + path);
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
        || ::listen(fd, 64) != 0) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        setError(error, "listen(" + path + ") failed: " + reason);
        return false;
    }
    std::string permissionError;
    if (!logos::applySocketPerms(path, &permissionError)) {
        ::close(fd);
        ::unlink(path.c_str());
        setError(error, permissionError);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->path = path;
        m_impl->listener = fd;
        m_impl->running = true;
        m_impl->acceptExited = false;
    }
#endif
    auto state = m_impl;
    std::thread([state] { state->acceptLoop(); }).detach();
    return true;
}

void Server::stop()
{
    std::vector<std::shared_ptr<Impl::Connection>> connections;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (!m_impl->running && m_impl->listener.load() == kInvalidHandle) return;
        m_impl->running = false;
        connections = m_impl->connections;
        path = m_impl->path;
    }
    closeHandle(m_impl->listener);
    for (const auto& connection : connections) closeHandle(connection->fd);
    std::unique_lock<std::mutex> lock(m_impl->mu);
    m_impl->changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return m_impl->acceptExited && m_impl->connections.empty();
    });
    lock.unlock();
#ifndef _WIN32
    if (!path.empty()) ::unlink(path.c_str());
#endif
}

bool Server::isRunning() const
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->running;
}

bool Server::publish(Object object, std::string* error)
{
    if (object.name.empty() || object.definition.typeName.empty() || !object.invoke) {
        setError(error, "published QtRO object requires name, type and invoke handler");
        return false;
    }
    std::vector<std::shared_ptr<Impl::Connection>> connections;
    ObjectInfo info{object.name, std::nullopt, {}};
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (m_impl->objects.count(object.name)) {
            setError(error, "QtRO object is already published: " + object.name);
            return false;
        }
        m_impl->objects.emplace(object.name, std::move(object));
        connections = m_impl->connections;
    }
    for (const auto& connection : connections)
        (void)m_impl->send(connection, objectListPacket({info}));
    return true;
}

void Server::unpublish(const std::string& name)
{
    std::vector<std::shared_ptr<Impl::Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (!m_impl->objects.erase(name)) return;
        connections = m_impl->connections;
        for (const auto& connection : connections)
            connection->acquired.erase(name);
    }
    for (const auto& connection : connections)
        (void)m_impl->send(connection, removeObjectPacket(name));
}

bool Server::emitSignal(const std::string& object,
                        std::int32_t signalIndex,
                        const std::vector<Variant>& arguments,
                        std::string* error)
{
    std::vector<std::shared_ptr<Impl::Connection>> listeners;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        const auto found = m_impl->objects.find(object);
        if (found == m_impl->objects.end()) {
            setError(error, "QtRO object is not published: " + object);
            return false;
        }
        if (signalIndex < 0
            || static_cast<std::size_t>(signalIndex) >= found->second.definition.signalDefinitions.size()) {
            setError(error, "QtRO signal index is out of range");
            return false;
        }
        for (const auto& connection : m_impl->connections)
            if (connection->acquired.count(object)) listeners.push_back(connection);
    }
    const auto frame = invokePacket(object, 0, signalIndex, arguments);
    bool ok = true;
    for (const auto& connection : listeners)
        ok = m_impl->send(connection, frame, error) && ok;
    return ok;
}

std::string Server::socketPath() const
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->path;
}

} // namespace logos::qt_remote_plain
