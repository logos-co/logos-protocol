#include "logos_socket_paths.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace logos {

#ifdef _WIN32

// On Windows the local transport is backed by NAMED PIPES: QLocalServer maps a
// server name to \\.\pipe\<name>. Every assumption this file is built on is a
// unix-domain-socket assumption, and none of them survives the move:
//
//   * a pipe has no filesystem inode, so there is nothing to lstat, chown or
//     chmod — access is governed by a security descriptor supplied at
//     CreateNamedPipe time, not by mode bits;
//   * a pipe instance ceases to exist when its last handle closes, so a
//     hard-killed process cannot leave a stale endpoint behind. There is no
//     equivalent of the /tmp/logos_<name>_<instance> litter the reaper exists
//     to clean up.
//
// So isSocketDead/reapStaleSockets are not merely unimplemented here, they are
// vacuous: the condition they detect cannot arise.

bool applySocketPerms(const std::string& absPath, std::string* errOut)
{
    const char* grpEnv = std::getenv("LOGOS_SOCKET_GROUP");
    const char* modeEnv = std::getenv("LOGOS_SOCKET_MODE");
    const bool wantGroup = grpEnv && *grpEnv;
    const bool wantMode = modeEnv && *modeEnv;

    // Policy unset is the overwhelmingly common case and is a genuine no-op.
    if (!wantGroup && !wantMode) return true;

    // Policy SET, though, is a request we cannot honour. Returning true here
    // would silently widen access relative to what the operator asked for --
    // the one direction this file is careful never to go (see the chgrp-then-
    // chmod ordering in the POSIX branch). Granting a Windows pipe to a group
    // means building a DACL and resolving the group to a SID; until that
    // exists, refuse loudly rather than pretend.
    if (errOut) {
        *errOut = "LOGOS_SOCKET_GROUP/LOGOS_SOCKET_MODE are not supported on "
                  "Windows: the local transport uses named pipes, which carry a "
                  "security descriptor rather than owner/group/mode. Unset them, "
                  "or run the node per-user (%LOCALAPPDATA%). Path: " + absPath;
    }
    return false;
}

bool isSocketDead(const std::string& /*absPath*/)
{
    // Fail closed, matching the documented contract: never report an endpoint
    // dead unless certain. A named pipe that still exists still has an owner.
    return false;
}

std::size_t reapStaleSockets(const std::string& /*dir*/, const std::string& /*prefix*/)
{
    return 0;  // named pipes leave nothing behind to reap
}

#else

namespace {

// Resolve a "group" env value to a gid. Accepts an all-digits string as a
// numeric gid directly, otherwise looks the name up in the group database.
bool resolveGid(const std::string& spec, gid_t& out)
{
    if (!spec.empty() &&
        spec.find_first_not_of("0123456789") == std::string::npos) {
        errno = 0;
        char* end = nullptr;
        const unsigned long v = std::strtoul(spec.c_str(), &end, 10);
        // Reject overflow and any value that doesn't fit gid_t — a truncated
        // gid would silently chgrp to the wrong group.
        if (errno != 0 || end == spec.c_str() || *end != '\0' ||
            v > static_cast<unsigned long>(std::numeric_limits<gid_t>::max()))
            return false;
        out = static_cast<gid_t>(v);
        return true;
    }

    // getgrnam_r with a growing buffer — thread-safe, unlike getgrnam.
    std::vector<char> buf(1024);
    struct group grp;
    struct group* result = nullptr;
    for (;;) {
        int rc = ::getgrnam_r(spec.c_str(), &grp, buf.data(), buf.size(), &result);
        if (rc == ERANGE && buf.size() < (1u << 20)) {
            buf.resize(buf.size() * 2);
            continue;
        }
        if (rc != 0 || result == nullptr) return false;
        out = grp.gr_gid;
        return true;
    }
}

// Parse an octal mode like "0660" / "660". Rejects garbage and anything wider
// than the low 12 bits (setuid/setgid/sticky + rwx triplets).
bool parseOctalMode(const std::string& spec, mode_t& out)
{
    if (spec.empty()) return false;
    for (char c : spec) {
        if (c < '0' || c > '7') return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(spec.c_str(), &end, 8);
    if (errno != 0 || end == spec.c_str() || *end != '\0' || v > 07777) return false;
    out = static_cast<mode_t>(v);
    return true;
}

}  // namespace

bool applySocketPerms(const std::string& absPath, std::string* errOut)
{
    auto fail = [&](const std::string& msg) {
        if (errOut) *errOut = msg;
        return false;
    };

    const char* grpEnv  = std::getenv("LOGOS_SOCKET_GROUP");
    const char* modeEnv = std::getenv("LOGOS_SOCKET_MODE");
    const bool wantGroup = grpEnv && *grpEnv;
    const bool wantMode  = modeEnv && *modeEnv;
    if (!wantGroup && !wantMode) return true;  // policy unset: no-op, touch nothing

    // Only ever change a socket we own. If a malformed URL produced a path that
    // isn't the socket we just bound, refuse rather than chmod/chown a stray
    // file. (Mirrors isSocketDead's owner check.)
    struct stat st;
    if (::lstat(absPath.c_str(), &st) != 0)
        return fail("stat(" + absPath + ") failed: " + std::strerror(errno));
    if (!S_ISSOCK(st.st_mode))
        return fail(absPath + " is not a socket — refusing to change perms");
    if (st.st_uid != ::geteuid())
        return fail(absPath + " is not owned by us — refusing to change perms");

    if (wantGroup) {
        gid_t gid = 0;
        if (!resolveGid(grpEnv, gid))
            return fail(std::string("unknown group '") + grpEnv + "'");
        // Non-root may chgrp a file it owns to any group it belongs to.
        if (::chown(absPath.c_str(), static_cast<uid_t>(-1), gid) != 0)
            return fail("chown(" + absPath + ") failed: " + std::strerror(errno));
    }

    if (wantMode) {
        mode_t mode = 0;
        if (!parseOctalMode(modeEnv, mode))
            return fail(std::string("invalid LOGOS_SOCKET_MODE '") + modeEnv + "'");
        if (::chmod(absPath.c_str(), mode) != 0)
            return fail("chmod(" + absPath + ") failed: " + std::strerror(errno));
    }

    return true;
}

bool isSocketDead(const std::string& absPath)
{
    struct stat st;
    if (::lstat(absPath.c_str(), &st) != 0) return false;   // gone / unreadable
    if (!S_ISSOCK(st.st_mode)) return false;                // regular file, dir, ...
    if (st.st_uid != ::geteuid()) return false;             // not ours to reap

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (absPath.size() >= sizeof(addr.sun_path)) return false;  // can't probe -> assume alive
    std::memcpy(addr.sun_path, absPath.c_str(), absPath.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int err = errno;
    ::close(fd);

    if (rc == 0) return false;                              // a listener answered -> alive
    // ECONNREFUSED: bound but nobody listening. ENOENT: vanished mid-probe.
    // Everything else (EAGAIN/EINPROGRESS backlog full, EACCES, ETIMEDOUT, ...)
    // is treated as alive so we never unlink a socket that might be in use.
    return err == ECONNREFUSED || err == ENOENT;
}

std::size_t reapStaleSockets(const std::string& dir, const std::string& prefix)
{
    // Refuse an empty prefix: it would make every dead socket the process owns
    // (anywhere in `dir`) a deletion candidate. Callers always know the family
    // of sockets they created ("logos_"), so this is misuse, not a valid sweep.
    if (prefix.empty()) return 0;

    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;

    std::size_t removed = 0;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.size() < prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string full = dir + "/" + name;
        if (isSocketDead(full) && ::unlink(full.c_str()) == 0)
            ++removed;
    }
    ::closedir(d);
    return removed;
}

#endif  // _WIN32

}  // namespace logos
