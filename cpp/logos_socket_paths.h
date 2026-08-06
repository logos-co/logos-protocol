#ifndef LOGOS_SOCKET_PATHS_H
#define LOGOS_SOCKET_PATHS_H

#include <cstddef>
#include <string>

// Qt-free helpers for making the unix-domain sockets that back the local
// transport shareable across OS users, and for cleaning up the stale socket
// files that a hard-killed process leaves behind. Depends on nothing but POSIX
// (or, on Windows, nothing at all) plus the C++ standard library, so it can be
// used from both the Qt (qt_remote) and the Qt-free (plain) transport code
// paths, and unit-tested on its own.
//
// ON WINDOWS the local transport is named pipes, not unix sockets, and every
// one of these operations changes character — see the block comment at the top
// of the .cpp. In short: there is no inode to chmod, and a pipe cannot outlive
// its last handle, so there is never anything stale to reap. Each function's
// Windows behaviour is called out below.
//
// The policy is read from the environment so every process in a Logos node's
// tree — the daemon, the module subprocesses (logos_host), any child it spawns
// — applies the same rule to every socket it binds without the config having to
// be threaded through each layer. The daemon exports the vars once next to
// LOGOS_INSTANCE_ID and they propagate by inheritance.
namespace logos {

// Apply the socket-access policy to an already-bound unix-domain socket file at
// the ABSOLUTE path `absPath`. Reads two environment variables:
//
//   LOGOS_SOCKET_GROUP — group name or numeric gid; the socket is chgrp'd to it.
//   LOGOS_SOCKET_MODE  — octal mode (e.g. "0660"); the socket is chmod'd to it.
//
// Order is chgrp-then-chmod so the transient state is never MORE permissive
// than the final target (a half-applied policy can only ever be too strict,
// never too loose — it opens nothing). An unset/empty var leaves that attribute
// untouched, so the default behaviour (no env) is a no-op and the socket keeps
// the kernel's `0777 & ~umask` mode. Connecting to an AF_UNIX socket needs
// WRITE permission on the file, so 0660 is what grants a group member access;
// the execute bit on a socket inode is inert.
//
// Returns true when every requested change succeeded (or nothing was
// requested). On failure returns false and, if `errOut` is non-null, writes a
// human-readable reason.
//
// Windows: returns true when no policy is requested, and FAILS with an
// explanatory error when LOGOS_SOCKET_GROUP/LOGOS_SOCKET_MODE are set --
// honouring them would need a pipe DACL and a group->SID resolver. Refusing
// loudly is deliberate: silently returning true would leave the endpoint more
// permissive than the operator asked for.
bool applySocketPerms(const std::string& absPath, std::string* errOut = nullptr);

// True iff `absPath` is a unix socket that WE own and whose listener is
// provably gone. Requires all of: the path exists and is a socket (S_ISSOCK),
// its owner is our effective uid, and a non-blocking connect() returns
// ECONNREFUSED or ENOENT. Any other outcome — a successful connect, EACCES,
// EAGAIN, ETIMEDOUT, a non-socket inode, a foreign owner — is treated as ALIVE
// and returns false. The predicate fails closed: it never reports a socket dead
// unless it is sure, so a reaper built on it cannot unlink a live endpoint or a
// regular file that merely shares the name.
//
// Windows: always false. A named pipe stops existing when its last handle
// closes, so a "dead but present" endpoint is not a state that can occur.
bool isSocketDead(const std::string& absPath);

// Unlink every entry `<dir>/<prefix>...` for which isSocketDead() returns true.
// Regular files are never removed (only S_ISSOCK inodes pass the predicate), so
// a large build artefact that happens to match the prefix is safe. Returns the
// number of socket files actually unlinked. A missing/unreadable `dir` yields 0.
//
// Windows: always 0 -- nothing is ever left behind to reap.
std::size_t reapStaleSockets(const std::string& dir, const std::string& prefix);

}  // namespace logos

#endif  // LOGOS_SOCKET_PATHS_H
