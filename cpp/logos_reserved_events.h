#ifndef LOGOS_RESERVED_EVENTS_H
#define LOGOS_RESERVED_EVENTS_H

#include <string>

// Event names the protocol keeps for its own plumbing. They travel over the SAME
// channel a module uses for its own events (see logos_async_dispatch.h), so
// without a reservation they are part of every module's event surface: a
// subscriber could ask for one by name, and a WILDCARD subscriber received every
// one of them — which for the completion event is every method's return value.
//
// Qt-free on purpose: the plain transport enforces the same reservation and does
// not link Qt. logos_async_dispatch.h adds the QString-flavoured predicate.
namespace logos {

inline constexpr const char* kCallCompleteEvent = "__logos_call_complete__";

inline bool isReservedEventName(const std::string& name)
{
    return name == kCallCompleteEvent;
}

} // namespace logos

#endif // LOGOS_RESERVED_EVENTS_H
