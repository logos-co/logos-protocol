// Unit tests for the Qt-free socket-access + stale-socket-reaper helpers
// (cpp/logos_socket_paths.{h,cpp}). These back the multi-user local transport:
// applySocketPerms makes a bound socket group-connectable, and
// isSocketDead/reapStaleSockets clean up the files a hard-killed process leaves
// behind without ever touching a live socket or an unrelated regular file.

#include "logos_socket_paths.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// A short, unique path under $TMPDIR (fallback /tmp). The AF_UNIX sun_path cap
// (~104 bytes) means we can't use a deep nix build dir, so keep the tag short.
std::string sockPath(const char* tag)
{
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir + "/lsp_" + tag + "_" + std::to_string(::getpid()) + ".sock";
}

// bind() a listening AF_UNIX socket at `path`; returns the fd (>=0) or -1.
int bindListening(const std::string& path)
{
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) { ::close(fd); return -1; }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) { ::close(fd); ::unlink(path.c_str()); return -1; }
    return fd;
}

struct EnvGuard {
    const char* name;
    std::string prev;
    bool had;
    EnvGuard(const char* n, const char* v) : name(n) {
        const char* p = std::getenv(n);
        had = p != nullptr;
        if (had) prev = p;
        if (v) ::setenv(n, v, 1); else ::unsetenv(n);
    }
    ~EnvGuard() {
        if (had) ::setenv(name, prev.c_str(), 1); else ::unsetenv(name);
    }
};

}  // namespace

// applySocketPerms with the mode env var set chmods the socket to that mode.
TEST(SocketPaths, AppliesModeFromEnv)
{
    const std::string path = sockPath("mode");
    int fd = bindListening(path);
    ASSERT_GE(fd, 0);

    {
        EnvGuard mode("LOGOS_SOCKET_MODE", "0660");
        EnvGuard grp("LOGOS_SOCKET_GROUP", nullptr);
        std::string err;
        EXPECT_TRUE(logos::applySocketPerms(path, &err)) << err;
    }

    struct stat st;
    ASSERT_EQ(::lstat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0660u);

    ::close(fd);
    ::unlink(path.c_str());
}

// chgrp to our own effective gid always succeeds (we are a member), so it is a
// stable, sandbox-safe way to prove the chown path runs.
TEST(SocketPaths, AppliesGroupFromEnv)
{
    const std::string path = sockPath("grp");
    int fd = bindListening(path);
    ASSERT_GE(fd, 0);

    const gid_t gid = ::getegid();
    {
        EnvGuard grp("LOGOS_SOCKET_GROUP", std::to_string(gid).c_str());
        EnvGuard mode("LOGOS_SOCKET_MODE", "0660");
        std::string err;
        EXPECT_TRUE(logos::applySocketPerms(path, &err)) << err;
    }

    struct stat st;
    ASSERT_EQ(::lstat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_gid, gid);

    ::close(fd);
    ::unlink(path.c_str());
}

// No env vars set -> no-op success, socket keeps its default mode.
TEST(SocketPaths, NoEnvIsNoOp)
{
    const std::string path = sockPath("noop");
    int fd = bindListening(path);
    ASSERT_GE(fd, 0);

    EnvGuard grp("LOGOS_SOCKET_GROUP", nullptr);
    EnvGuard mode("LOGOS_SOCKET_MODE", nullptr);
    EXPECT_TRUE(logos::applySocketPerms(path));

    ::close(fd);
    ::unlink(path.c_str());
}

// With the policy set, a path that isn't a socket we own is refused rather than
// chmod'd — guards against a malformed URL producing a stray path.
TEST(SocketPaths, RefusesNonSocketPath)
{
    const std::string path = sockPath("notsock");
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    ASSERT_GE(fd, 0);
    ::close(fd);

    EnvGuard grp("LOGOS_SOCKET_GROUP", nullptr);
    EnvGuard mode("LOGOS_SOCKET_MODE", "0660");
    std::string err;
    EXPECT_FALSE(logos::applySocketPerms(path, &err));
    EXPECT_FALSE(err.empty());

    // The file's mode is unchanged (still 0644, not 0660).
    struct stat st;
    ASSERT_EQ(::lstat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0644u);
    ::unlink(path.c_str());
}

// A malformed mode is rejected (returns false) and changes nothing.
TEST(SocketPaths, RejectsBadMode)
{
    const std::string path = sockPath("badmode");
    int fd = bindListening(path);
    ASSERT_GE(fd, 0);

    EnvGuard grp("LOGOS_SOCKET_GROUP", nullptr);
    EnvGuard mode("LOGOS_SOCKET_MODE", "not-octal");
    std::string err;
    EXPECT_FALSE(logos::applySocketPerms(path, &err));
    EXPECT_FALSE(err.empty());

    ::close(fd);
    ::unlink(path.c_str());
}

// A socket with a live listener is NOT dead.
TEST(SocketPaths, LiveSocketIsNotDead)
{
    const std::string path = sockPath("live");
    int fd = bindListening(path);
    ASSERT_GE(fd, 0);

    EXPECT_FALSE(logos::isSocketDead(path));

    ::close(fd);
    ::unlink(path.c_str());
}

// After the listener closes, the leftover socket file IS dead (connect ->
// ECONNREFUSED). This is exactly the leaked-socket case.
TEST(SocketPaths, ClosedSocketIsDead)
{
    const std::string path = sockPath("dead");
    int fd = bindListening(path);
    ASSERT_GE(fd, 0);
    ::close(fd);  // file remains on disk, no listener

    EXPECT_TRUE(logos::isSocketDead(path));
    ::unlink(path.c_str());
}

// Detector (Darwin): a live listener whose backlog is full also refuses with
// ECONNREFUSED; one probe read it as dead, so a new server replaced it.
TEST(SocketPaths, ABusyListenerWithAFullBacklogIsNotDead)
{
    const std::string path = sockPath("busy");
    ::unlink(path.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);

    // Fill the backlog until the kernel refuses.
    std::vector<int> pending;
    for (int i = 0; i < 16; ++i) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
            && errno != EINPROGRESS) {
            ::close(fd);
            break;
        }
        pending.push_back(fd);
    }
    // The owner is alive: it gets round to accepting shortly.
    std::thread drain([listener, count = pending.size()] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        for (std::size_t i = 0; i < count; ++i) {
            const int fd = ::accept(listener, nullptr, nullptr);
            if (fd >= 0) ::close(fd);
        }
    });
    EXPECT_FALSE(logos::isSocketDead(path));
    drain.join();
    for (int fd : pending) ::close(fd);
    ::close(listener);
    ::unlink(path.c_str());
}

// A regular file that merely shares the naming prefix is never "dead" — the
// reaper must not delete a build artefact like logos_execution_zone-1.0.0.lgx.
TEST(SocketPaths, RegularFileIsNeverDead)
{
    const std::string path = sockPath("regular");
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    ASSERT_GE(fd, 0);
    ::write(fd, "not a socket", 12);
    ::close(fd);

    EXPECT_FALSE(logos::isSocketDead(path));
    ::unlink(path.c_str());
}

// reapStaleSockets removes only the dead socket, keeps the live one, and never
// touches the regular file.
TEST(SocketPaths, ReaperRemovesOnlyDeadSockets)
{
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    if (!dir.empty() && dir.back() == '/') dir.pop_back();

    const std::string prefix = "lspreap_" + std::to_string(::getpid()) + "_";
    const std::string deadPath = dir + "/" + prefix + "dead.sock";
    const std::string livePath = dir + "/" + prefix + "live.sock";
    const std::string filePath = dir + "/" + prefix + "file";

    int deadFd = bindListening(deadPath);
    ASSERT_GE(deadFd, 0);
    ::close(deadFd);  // leaves a stale socket file

    int liveFd = bindListening(livePath);
    ASSERT_GE(liveFd, 0);  // keep listening

    ::unlink(filePath.c_str());
    int rf = ::open(filePath.c_str(), O_CREAT | O_WRONLY, 0644);
    ASSERT_GE(rf, 0);
    ::close(rf);

    // An empty prefix is refused outright (would sweep every dead socket we
    // own in dir) — nothing removed.
    EXPECT_EQ(logos::reapStaleSockets(dir, ""), 0u);

    const std::size_t removed = logos::reapStaleSockets(dir, prefix);
    EXPECT_EQ(removed, 1u);

    struct stat st;
    EXPECT_NE(::lstat(deadPath.c_str(), &st), 0);  // dead socket gone
    EXPECT_EQ(::lstat(livePath.c_str(), &st), 0);  // live socket kept
    EXPECT_EQ(::lstat(filePath.c_str(), &st), 0);  // regular file kept

    ::close(liveFd);
    ::unlink(livePath.c_str());
    ::unlink(filePath.c_str());
}
