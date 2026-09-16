#ifndef LOGOS_OBJECT_SOURCE_WATCH_H
#define LOGOS_OBJECT_SOURCE_WATCH_H

#include <functional>

// INTERNAL (not installed): a handle that knows when the source it was bound to went away, even if
// the transport has since re-attached it to a replacement. Reached with dynamic_cast, like LogosObjectErrorChannel.
class LogosObjectSourceWatch {
public:
    virtual ~LogosObjectSourceWatch() = default;

    // Latched: true once the bound source went away (or was already gone at bind time).
    virtual bool sourceLost() const = 0;

    // Deliver user events only from the current source; `onLost` runs inline inside the transport, so it must only defer.
    virtual void bindToSource(std::function<void()> onLost) = 0;
};

#endif // LOGOS_OBJECT_SOURCE_WATCH_H
