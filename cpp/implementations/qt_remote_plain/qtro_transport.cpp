#include "qtro_transport.h"

#include "../../logos_socket_paths.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
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
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace logos::qt_remote_plain {
namespace {

constexpr std::size_t kMaximumFrame = 64u << 20;
using Deadline = std::chrono::steady_clock::time_point;
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
                     DWORD& written, std::string* error,
                     const std::optional<Deadline>& deadline)
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
            DWORD wait = INFINITE;
            if (deadline) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now()).count();
                wait = static_cast<DWORD>(std::max<std::int64_t>(0,
                    std::min<std::int64_t>(remaining, MAXDWORD - 1)));
            }
            const DWORD result = ::WaitForSingleObject(operation.hEvent, wait);
            if (result != WAIT_OBJECT_0) {
                const DWORD waitError = result == WAIT_FAILED ? ::GetLastError() : 0;
                // The OVERLAPPED and its buffer must remain alive until the
                // cancelled operation completes, even when the deadline ends.
                (void)::CancelIoEx(handle, &operation);
                (void)::WaitForSingleObject(operation.hEvent, INFINITE);
                setError(error, result == WAIT_TIMEOUT
                    ? "local socket write timed out"
                    : windowsError("WaitForSingleObject", waitError));
                success = false;
            } else {
                success = ::GetOverlappedResult(handle, &operation, &written, FALSE) != FALSE;
                if (!success)
                    setError(error, windowsError("WriteFile", ::GetLastError()));
            }
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
              std::string* error, const std::optional<Deadline>& deadline = {})
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            setError(error, "local socket write timed out");
            return false;
        }
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(
            data.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!overlappedWrite(winHandle(handle), data.data() + offset, remaining,
                             written, error, deadline) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool readExact(NativeHandle handle, std::uint8_t* output, std::size_t size,
               const std::atomic<bool>* running)
{
    (void)running;
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

int pollTimeout(const Deadline& deadline)
{
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return 0;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining) ++milliseconds;
    return static_cast<int>(std::min<std::int64_t>(milliseconds.count(),
        std::numeric_limits<int>::max()));
}

bool waitForSocket(int fd, short events, const Deadline& deadline,
                   std::string* error, const char* operation)
{
    pollfd descriptor{fd, events, 0};
    for (;;) {
        const int timeout = pollTimeout(deadline);
        if (timeout == 0) {
            setError(error, std::string(operation) + " timed out");
            return false;
        }
        const int result = ::poll(&descriptor, 1, timeout);
        if (result > 0) return true;
        if (result == 0) {
            setError(error, std::string(operation) + " timed out");
            return false;
        }
        if (errno != EINTR) {
            setError(error, std::string(operation) + " failed: " + std::strerror(errno));
            return false;
        }
    }
}

bool writeAll(NativeHandle fd, const std::vector<std::uint8_t>& data,
              std::string* error, const std::optional<Deadline>& deadline = {})
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            setError(error, "local socket write timed out");
            return false;
        }
#ifdef MSG_NOSIGNAL
        constexpr int signalFlag = MSG_NOSIGNAL;
#else
        constexpr int signalFlag = 0;
#endif
        const int flags = signalFlag | (deadline ? MSG_DONTWAIT : 0);
        const auto count = ::send(fd, data.data() + offset, data.size() - offset, flags);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (deadline) {
                if (!waitForSocket(static_cast<int>(fd), POLLOUT, *deadline, error,
                                   "local socket write")) return false;
            } else {
                pollfd descriptor{static_cast<int>(fd), POLLOUT, 0};
                if (::poll(&descriptor, 1, -1) < 0 && errno != EINTR) {
                    setError(error, "local socket write failed: "
                        + std::string(std::strerror(errno)));
                    return false;
                }
            }
            continue;
        }
        if (count <= 0) {
            setError(error, count == 0 ? "local socket closed while writing"
                : "local socket write failed: " + std::string(std::strerror(errno)));
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool readExact(NativeHandle fd, std::uint8_t* output, std::size_t size,
               const std::atomic<bool>* running)
{
    std::size_t offset = 0;
    while (offset < size) {
        if (running && !running->load()) return false;
        const auto count = ::recv(fd, output + offset, size - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{static_cast<int>(fd), POLLIN, 0};
            if (::poll(&descriptor, 1, 50) < 0 && errno != EINTR) return false;
            continue;
        }
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

#endif

// A frame above kMaximumFrame is discarded rather than buffered. Its head
// still names the packet and object (and an InvokeReply's serial); its last
// eight bytes are an Invoke's serial and property index.
struct OversizedFrame {
    bool present = false;
    std::uint32_t size = 0;
    std::uint8_t tail[8] = {};

    std::int32_t tailSerial() const
    {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(tail[0])
            | (static_cast<std::uint32_t>(tail[1]) << 8)
            | (static_cast<std::uint32_t>(tail[2]) << 16)
            | (static_cast<std::uint32_t>(tail[3]) << 24));
    }
};

constexpr std::size_t kOversizedHead = 64u << 10;

bool readPacket(NativeHandle handle, std::vector<std::uint8_t>& output,
                std::string* error, const std::atomic<bool>* running,
                OversizedFrame* oversized)
{
    oversized->present = false;
    output.resize(4);
    if (!readExact(handle, output.data(), 4, running)) {
        setError(error, "local socket closed");
        return false;
    }
    const std::uint32_t payload = static_cast<std::uint32_t>(output[0])
        | (static_cast<std::uint32_t>(output[1]) << 8)
        | (static_cast<std::uint32_t>(output[2]) << 16)
        | (static_cast<std::uint32_t>(output[3]) << 24);
    const std::uint32_t kept = payload > kMaximumFrame
        ? static_cast<std::uint32_t>(kOversizedHead) : payload;
    output.resize(4 + static_cast<std::size_t>(kept));
    if (!readExact(handle, output.data() + 4, kept, running)) {
        setError(error, "local socket closed in the middle of a frame");
        return false;
    }
    if (payload == kept) return true;

    oversized->present = true;
    oversized->size = payload;
    std::memcpy(oversized->tail, output.data() + output.size() - 8, 8);
    std::vector<std::uint8_t> scratch(kOversizedHead);
    std::uint64_t left = static_cast<std::uint64_t>(payload) - kept;
    while (left > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, scratch.size()));
        if (!readExact(handle, scratch.data(), chunk, running)) {
            setError(error, "local socket closed in the middle of a frame");
            return false;
        }
        if (chunk >= 8) {
            std::memcpy(oversized->tail, scratch.data() + chunk - 8, 8);
        } else {
            std::memmove(oversized->tail, oversized->tail + chunk, 8 - chunk);
            std::memcpy(oversized->tail + 8 - chunk, scratch.data(), chunk);
        }
        left -= chunk;
    }
    output[0] = static_cast<std::uint8_t>(kept);
    output[1] = static_cast<std::uint8_t>(kept >> 8);
    output[2] = static_cast<std::uint8_t>(kept >> 16);
    output[3] = static_cast<std::uint8_t>(kept >> 24);
    return true;
}

// An Invoke's serial is its second-to-last field; readable even when an
// argument before it cannot be decoded.
std::int32_t trailingSerial(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 8) return -1;
    const std::size_t at = payload.size() - 8;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(payload[at])
        | (static_cast<std::uint32_t>(payload[at + 1]) << 8)
        | (static_cast<std::uint32_t>(payload[at + 2]) << 16)
        | (static_cast<std::uint32_t>(payload[at + 3]) << 24));
}

struct InvokeFrame {
    std::int32_t call = -1;
    std::int32_t index = -1;
    std::vector<Variant> arguments;
    std::int32_t serial = -1;
};

// The last argument ends where the serial begins, so a value the codec cannot
// interpret there is kept opaque instead of failing the frame.
InvokeFrame decodeInvoke(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 20) throw CodecError("truncated QtRO invoke");
    Reader reader(payload);
    InvokeFrame frame;
    frame.call = reader.i32();
    frame.index = reader.i32();
    const auto count = reader.u32();
    if (count > 1024) throw CodecError("QtRO invoke has too many arguments");
    frame.arguments.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        frame.arguments.push_back(i + 1 == count ? reader.variantUntil(payload.size() - 8)
                                                 : reader.variant());
    frame.serial = reader.i32();
    (void)reader.i32();
    return frame;
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
                           std::chrono::milliseconds timeout,
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
    const int originalFlags = ::fcntl(fd, F_GETFL, 0);
    if (originalFlags < 0 || ::fcntl(fd, F_SETFL, originalFlags | O_NONBLOCK) < 0) {
        setError(error, "fcntl() failed: " + std::string(std::strerror(errno)));
        ::close(fd);
        return kInvalidHandle;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int connectError = errno;
        if (connectError != EINPROGRESS || !waitForSocket(fd, POLLOUT, deadline, error,
                                                           "local socket connection")) {
            if (connectError != EINPROGRESS)
                setError(error, "connect(" + path + ") failed: " + std::string(std::strerror(connectError)));
            ::close(fd);
            return kInvalidHandle;
        }
        int result = 0;
        socklen_t length = sizeof(result);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &length) < 0 || result != 0) {
            setError(error, "connect(" + path + ") failed: "
                + std::string(std::strerror(result == 0 ? errno : result)));
            ::close(fd);
            return kInvalidHandle;
        }
    }
    // Keep client sockets nonblocking: Darwin does not reliably honor
    // MSG_DONTWAIT on local stream sends when the peer stops reading.
    // readExact handles EAGAIN by waiting for readable data.
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
    std::string temp = configured && *configured ? configured : "";
#ifdef __APPLE__
    // QLocalServer uses QDir::tempPath() for relative names. On macOS that
    // falls back to the per-user Darwin temp directory when TMPDIR is absent,
    // not /tmp. A daemon launched by a service may have no TMPDIR even when an
    // interactive shell does, so the two sides must make the same choice.
    if (temp.empty()) {
        const std::size_t required = ::confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
        if (required > 1) {
            std::string buffer(required, '\0');
            if (::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer.data(), required) > 0) {
                buffer.resize(std::strlen(buffer.c_str()));
                temp = std::move(buffer);
            }
        }
    }
#endif
    if (temp.empty()) temp = "/tmp";
    while (temp.size() > 1 && temp.back() == '/') temp.pop_back();
    return temp + "/" + std::string(localUrlOrPath);
#endif
}

namespace {

// QtRO delivers signals independently of the socket reader. A callback may
// synchronously call the same remote object, so the reader must remain free to
// consume that call's reply while the callback is running.
struct CallbackExecutor : std::enable_shared_from_this<CallbackExecutor> {
    std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable exitedChanged;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    bool exited = false;
    std::thread::id worker;

    static std::shared_ptr<CallbackExecutor> start()
    {
        auto executor = std::make_shared<CallbackExecutor>();
        std::thread([executor] { executor->run(); }).detach();
        return executor;
    }

    bool post(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return false;
            jobs.push_back(std::move(job));
        }
        changed.notify_one();
        return true;
    }

    void requestStop()
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        jobs.clear();
        changed.notify_all();
    }

    void waitStopped()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (worker == std::this_thread::get_id()) return;
        exitedChanged.wait(lock, [&] { return exited; });
    }

    void stop()
    {
        requestStop();
        waitStopped();
    }

    void run()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            worker = std::this_thread::get_id();
        }
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping) break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            try {
                job();
            } catch (...) {
                // A user callback must not terminate the transport worker.
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            exited = true;
        }
        exitedChanged.notify_all();
    }
};

} // namespace

struct Client::Impl : std::enable_shared_from_this<Client::Impl> {
    struct Pending {
        std::condition_variable cv;
        bool done = false;
        Variant value;
        std::string error;
    };

    mutable std::mutex mu;
    std::timed_mutex sendMu;
    std::condition_variable changed;
    std::atomic<NativeHandle> fd{kInvalidHandle};
    std::atomic<bool> running{false};
    bool readerExited = true;
    bool handshake = false;
    bool objectListSeen = false;
    std::string failure;
    std::map<std::string, ObjectInfo> advertised;
    std::map<std::string, ClassDefinition> definitions;
    std::map<std::string, std::string> definitionFailures;
    std::set<std::string> requested;
    std::map<std::int32_t, std::shared_ptr<Pending>> pending;
    std::int32_t nextSerial = 1;
    EventHandler eventHandler;
    InternalEventHandler internalEventHandler;
    DisconnectHandler disconnectHandler;
    std::shared_ptr<CallbackExecutor> eventExecutor;
    std::vector<std::shared_ptr<CallbackExecutor>> retiredExecutors;
    bool disconnectReported = false;
    std::thread::id readerThread;

    bool send(const std::vector<std::uint8_t>& frame, std::string* error = nullptr,
              const std::optional<Deadline>& deadline = {})
    {
        std::unique_lock<std::timed_mutex> lock(sendMu, std::defer_lock);
        if (deadline) {
            if (!lock.try_lock_until(*deadline)) {
                setError(error, "local socket write timed out");
                return false;
            }
        } else {
            lock.lock();
        }
        const NativeHandle current = fd.load();
        if (current == kInvalidHandle) {
            setError(error, "local socket is closed");
            return false;
        }
        return writeAll(current, frame, error, deadline);
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

    void completeCall(std::int32_t serial, Variant value, std::string error)
    {
        std::shared_ptr<Pending> call;
        {
            std::lock_guard<std::mutex> lock(mu);
            const auto it = pending.find(serial);
            if (it == pending.end()) return;
            call = it->second;
            pending.erase(it);
            call->value = std::move(value);
            call->error = std::move(error);
            call->done = true;
        }
        call->cv.notify_all();
    }

    void handle(Frame frame, const OversizedFrame& oversized)
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
            try {
                Reader reader(frame.payload);
                auto definition = reader.classDefinition();
                const auto properties = reader.u32();
                for (std::uint32_t i = 0; i < properties; ++i) (void)reader.variant();
                std::lock_guard<std::mutex> lock(mu);
                definitions[frame.name] = std::move(definition);
                definitionFailures.erase(frame.name);
            } catch (const CodecError& exception) {
                // One unusable definition fails its acquire, not the connection.
                std::lock_guard<std::mutex> lock(mu);
                definitionFailures[frame.name] = exception.what();
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
            if (frame.payload.size() < 4) break;
            Reader reader(frame.payload);
            const auto serial = reader.i32();
            Variant value;
            std::string replyError;
            if (oversized.present) {
                replyError = "QtRO reply of " + std::to_string(oversized.size)
                    + " bytes exceeds the transport limit";
            } else {
                try {
                    value = reader.variantUntil(frame.payload.size());
                } catch (const CodecError& exception) {
                    replyError = std::string("undecodable QtRO reply: ") + exception.what();
                }
            }
            completeCall(serial, std::move(value), std::move(replyError));
            break;
        }
        case PacketType::Invoke: {
            if (oversized.present) break;
            InvokeFrame invoke;
            try {
                invoke = decodeInvoke(frame.payload);
            } catch (const CodecError&) {
                break; // one lost event, not a lost connection
            }
            const auto callKind = invoke.call;
            const auto index = invoke.index;
            std::vector<Variant> args = std::move(invoke.arguments);
            EventHandler callback;
            InternalEventHandler internalCallback;
            std::shared_ptr<CallbackExecutor> executor;
            {
                std::lock_guard<std::mutex> lock(mu);
                callback = eventHandler;
                internalCallback = internalEventHandler;
                executor = eventExecutor;
            }
            const bool consumed = callKind == 0 && internalCallback
                && internalCallback(frame.name, index, args);
            if (callKind == 0 && !consumed && callback && executor) {
                const std::string object = std::move(frame.name);
                executor->post([callback = std::move(callback), object,
                                index, args = std::move(args)]() mutable {
                    callback(object, index, std::move(args));
                });
            }
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
        OversizedFrame oversized;
        while (running && readPacket(fd.load(), bytes, &error, &running, &oversized)) {
            try {
                handle(decodeFrame(bytes), oversized);
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
        {
            std::lock_guard<std::mutex> lock(mu);
            readerExited = true;
        }
        changed.notify_all();
        // User/status handling may wait behind a running event callback, and
        // that callback is allowed to reconnect. Release close()/connect()
        // before invoking the handler, otherwise it and the callback can wait
        // on each other forever.
        if (disconnected) disconnected(error);
    }
};

Client::Client() : m_impl(std::make_shared<Impl>()) {}
Client::~Client() { close(); }

bool Client::connect(const std::string& localUrlOrPath,
                     std::chrono::milliseconds timeout,
                     std::string* error)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    closeConnection(false);
    const std::string path = localSocketPath(localUrlOrPath);
    const NativeHandle socket = connectSocket(path, timeout, error);
    if (socket == kInvalidHandle) return false;
    auto executor = CallbackExecutor::start();
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
        m_impl->definitionFailures.clear();
        m_impl->requested.clear();
        m_impl->disconnectReported = false;
        m_impl->eventExecutor = std::move(executor);
    }
    auto state = m_impl;
    std::thread([state] { state->readLoop(); }).detach();

    std::unique_lock<std::mutex> lock(m_impl->mu);
    const bool ready = m_impl->changed.wait_until(lock, deadline, [&] {
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
    closeConnection(true);
}

void Client::closeWithoutWaitingForCallbacks()
{
    closeConnection(false);
}

void Client::closeConnection(bool waitForCallbacks)
{
    std::shared_ptr<CallbackExecutor> executor;
    std::vector<std::shared_ptr<CallbackExecutor>> retired;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (m_impl->readerExited && m_impl->fd.load() == kInvalidHandle
            && !m_impl->eventExecutor
            && (!waitForCallbacks || m_impl->retiredExecutors.empty())) return;
        m_impl->running = false;
        executor = std::move(m_impl->eventExecutor);
        if (waitForCallbacks)
            retired.swap(m_impl->retiredExecutors);
        else if (executor)
            m_impl->retiredExecutors.push_back(executor);
    }
    closeHandle(m_impl->fd);
    std::unique_lock<std::mutex> lock(m_impl->mu);
    if (m_impl->readerThread != std::this_thread::get_id())
        m_impl->changed.wait(lock, [&] { return m_impl->readerExited; });
    lock.unlock();
    if (executor) {
        executor->requestStop();
        // Reconnect must not wait for an event callback which may itself be the
        // caller waiting to acquire the reconnect serialization lock.
        if (waitForCallbacks) executor->waitStopped();
    }
    if (waitForCallbacks) {
        for (const auto& old : retired) old->stop();
    }
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
        const auto unusable = [&] {
            const auto it = m_impl->definitionFailures.find(object);
            if (it == m_impl->definitionFailures.end()) return false;
            setError(error, "unusable QtRO definition for " + object + ": " + it->second);
            return true;
        };
        if (unusable()) return false;
        if (m_impl->requested.insert(object).second) {
            lock.unlock();
            std::string writeError;
            if (!m_impl->send(addObjectPacket(object), &writeError, deadline)) {
                setError(error, writeError);
                closeConnection(false);
                return false;
            }
            lock.lock();
        }
        if (!m_impl->changed.wait_until(lock, deadline, [&] {
                return m_impl->definitions.count(object) != 0
                    || m_impl->definitionFailures.count(object) != 0
                    || !m_impl->failure.empty();
            })) {
            setError(error, "dynamic object definition timed out: " + object);
            return false;
        }
        if (!m_impl->failure.empty()) {
            setError(error, m_impl->failure);
            return false;
        }
        if (unusable()) return false;
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
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!acquire(object,
                 std::max(std::chrono::milliseconds::zero(),
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())),
                 error)) return std::nullopt;

    std::int32_t methodIndex = -1;
    std::int32_t serial = -1;
    auto pending = std::make_shared<Impl::Pending>();
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        // A RemoveObject can land between acquire() and here.
        const auto definition = m_impl->definitions.find(object);
        if (definition == m_impl->definitions.end()) {
            setError(error, "object was removed before the call: " + object);
            return std::nullopt;
        }
        const auto& methods = definition->second.methodDefinitions;
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

    std::vector<std::uint8_t> request;
    try {
        request = invokePacket(object, 0, methodIndex, arguments, serial);
    } catch (const CodecError& exception) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->pending.erase(serial);
        setError(error, std::string("cannot encode QtRO call: ") + exception.what());
        return std::nullopt;
    }
    std::string writeError;
    if (!m_impl->send(request, &writeError, deadline)) {
        {
            std::lock_guard<std::mutex> lock(m_impl->mu);
            m_impl->pending.erase(serial);
        }
        closeConnection(false);
        setError(error, writeError);
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(m_impl->mu);
    if (!pending->cv.wait_until(lock, deadline, [&] { return pending->done; })) {
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

void Client::setInternalEventHandler(InternalEventHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->internalEventHandler = std::move(handler);
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
        std::thread::id worker;
    };

    mutable std::mutex mu;
    std::condition_variable changed;
    std::atomic<NativeHandle> listener{kInvalidHandle};
    std::atomic<bool> running{false};
    bool acceptExited = true;
    std::size_t activeInvocations = 0;
    std::string path;
    std::map<std::string, Object> objects;
    std::vector<std::shared_ptr<Connection>> connections;

    // The caller holds connection.sendMu.
    static bool writeLocked(Connection& connection,
                            const std::vector<std::uint8_t>& frame,
                            std::string* error = nullptr)
    {
        const NativeHandle current = connection.fd.load();
        return current != kInvalidHandle && writeAll(current, frame, error);
    }

    bool send(const std::shared_ptr<Connection>& connection,
              const std::vector<std::uint8_t>& frame,
              std::string* error = nullptr)
    {
        std::lock_guard<std::mutex> lock(connection->sendMu);
        return writeLocked(*connection, frame, error);
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
        {
            std::lock_guard<std::mutex> lock(mu);
            connection->worker = std::this_thread::get_id();
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        OversizedFrame oversized;
        while (running && readPacket(connection->fd.load(), bytes, &error, &running, &oversized)) {
            try {
                const Frame frame = decodeFrame(bytes);
                if (oversized.present) {
                    // Answer the call with Qt's failure value; keep the connection.
                    const auto serial = oversized.tailSerial();
                    if (frame.type == PacketType::Invoke && serial >= 0)
                        (void)send(connection, invokeReplyPacket(frame.name, serial, Variant{}));
                    continue;
                }
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
                    if (frame.payload.size() != 1 || frame.payload[0] > 1) continue;
                    const bool dynamic = frame.payload[0] != 0;
                    // Register and answer under one send lock: an event that
                    // reaches a Qt replica before its definition crashes it.
                    std::lock_guard<std::mutex> sendLock(connection->sendMu);
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
                    if (found)
                        (void)writeLocked(*connection, dynamic
                            ? initDynamicPacket(frame.name, object.definition)
                            : initPacket(frame.name));
                    continue;
                }
                if (frame.type == PacketType::Invoke) {
                    InvokeFrame invoke;
                    try {
                        invoke = decodeInvoke(frame.payload);
                    } catch (const CodecError&) {
                        // Qt answers a call it cannot dispatch with an invalid
                        // QVariant; do the same rather than drop the connection.
                        const auto serial = trailingSerial(frame.payload);
                        if (serial >= 0)
                            (void)send(connection, invokeReplyPacket(frame.name, serial, Variant{}));
                        continue;
                    }
                    const auto callKind = invoke.call;
                    const auto methodIndex = invoke.index;
                    const auto serial = invoke.serial;
                    std::vector<Variant> arguments = std::move(invoke.arguments);
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
                        {
                            std::lock_guard<std::mutex> lock(mu);
                            ++activeInvocations;
                        }
                        auto state = shared_from_this();
                        const std::string objectName = std::move(frame.name);
                        try {
                            std::thread([state, connection, object = std::move(object),
                                         objectName, methodIndex, serial,
                                         arguments = std::move(arguments)]() mutable {
                                Variant result;
                                try {
                                    result = object.invoke(methodIndex, arguments);
                                } catch (...) {
                                    result = Variant::fromRpc(plain::RpcValue{});
                                }
                                if (serial >= 0) {
                                    std::vector<std::uint8_t> reply;
                                    try {
                                        reply = invokeReplyPacket(objectName, serial, result);
                                    } catch (const std::exception&) {
                                        reply = invokeReplyPacket(objectName, serial, Variant{});
                                    }
                                    (void)state->send(connection, reply);
                                }
                                {
                                    std::lock_guard<std::mutex> lock(state->mu);
                                    --state->activeInvocations;
                                }
                                state->changed.notify_all();
                            }).detach();
                        } catch (...) {
                            {
                                std::lock_guard<std::mutex> lock(mu);
                                --activeInvocations;
                            }
                            changed.notify_all();
                            if (serial >= 0)
                                (void)send(connection, invokeReplyPacket(
                                    objectName, serial,
                                    Variant::fromRpc(plain::RpcValue{})));
                        }
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
            // publish() sees the connection as soon as it is registered; hold
            // its send lock so nothing can precede the handshake.
            std::unique_lock<std::mutex> greeting(connection->sendMu);
            {
                std::lock_guard<std::mutex> lock(mu);
                if (!running) {
                    closeHandle(connection->fd);
                    break;
                }
                connections.push_back(connection);
                currentObjects = objectList();
            }
            const bool greeted = writeLocked(*connection, handshakePacket())
                && writeLocked(*connection, objectListPacket(currentObjects));
            greeting.unlock();
            if (!greeted) closeHandle(connection->fd);
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
        if (!m_impl->running && m_impl->listener.load() == kInvalidHandle
            && m_impl->connections.empty() && m_impl->activeInvocations == 0) return;
        m_impl->running = false;
        connections = m_impl->connections;
        path = m_impl->path;
    }
    closeHandle(m_impl->listener);
    for (const auto& connection : connections) closeHandle(connection->fd);
    std::unique_lock<std::mutex> lock(m_impl->mu);
    const auto caller = std::this_thread::get_id();
    m_impl->changed.wait(lock, [&] {
        return m_impl->acceptExited
            && m_impl->activeInvocations == 0
            && std::all_of(m_impl->connections.begin(), m_impl->connections.end(),
                [&](const auto& connection) { return connection->worker == caller; });
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
    std::vector<std::uint8_t> frame;
    try {
        frame = invokePacket(object, 0, signalIndex, arguments);
    } catch (const CodecError& exception) {
        setError(error, std::string("cannot encode QtRO signal: ") + exception.what());
        return false;
    }
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
