#ifndef LOGOS_SUBSCRIPTION_STATE_H
#define LOGOS_SUBSCRIPTION_STATE_H

/**
 * @brief Lifecycle of a subscription made with onEventWhenAvailable().
 *
 * Pending and Armed are both LIVE — the difference is only whether the module
 * has shown up yet. Unknown means the registry is not tracking that id at all:
 * it was cancelled, it was abandoned, or it never existed. A caller holding its
 * own "already subscribed" record and seeing Unknown is holding a stale one,
 * and short-circuiting on it would turn a legitimate re-subscribe into a
 * silent no-op.
 *
 * Lives in a header of its own so LogosAPIClient can name it without including
 * logos_api_consumer.h — it only forward-declares the consumer, deliberately.
 */
enum class LogosSubscriptionState {
    Unknown = 0,
    Pending,
    Armed,
    // Armed once, then lost its provider under a Manual restart policy. LIVE,
    // like Pending and Armed — the registry is still tracking it and
    // rearmSubscription() revives it. Appended, never inserted, so the numeric
    // values of the first three are unchanged.
    //
    // It is deliberately NOT Unknown: a caller holding its own "already
    // subscribed" record checks this against Unknown to detect a stale one
    // (logos-view-module-runtime/src/LogosQmlBridge.cpp:513), and answering
    // Unknown here would make it throw away a record that is perfectly good and
    // re-subscribe behind the policy's back.
    Held,
};

/**
 * @brief A transition worth telling the subscriber about.
 *
 * The state above answers "where is my subscription now?"; this answers "what
 * just happened to it?" — the difference that matters when a provider restarts.
 * A subscription that goes Lost and later Armed again is a NEW subscription:
 * events emitted in between reached nobody and cannot be recovered. Without
 * this edge a subscriber cannot distinguish that gap from a quiet module, which
 * is the whole reason it exists.
 *
 * Lost is NOT terminal — the registry keeps re-arming, exactly as it always
 * has. Abandoned is: the transport proved the object is unreachable on this
 * connection and the subscription will never fire again.
 */
enum class LogosSubscriptionEvent {
    Armed = 1,
    Lost,
    Abandoned,
    // Down, and this subscription's policy is Manual, so the registry will NOT
    // re-arm it. Replaces Lost rather than accompanying it: a subscriber gets
    // exactly one of the two, so "did it re-arm itself?" is answered by which
    // code arrived and never by inspecting policy from the callback.
    // Not terminal — rearmSubscription() revives it.
    Held,
};

// What the registry does when an ARMED subscription loses its provider.
//
// Automatic is what every subscription has always done and remains the default.
// Manual means "do not RE-arm after a loss" and NEVER "do not arm the first
// time": the deferred first arm is what makes a subscription taken during
// init() — before the provider has called listen() — work at all, and it is
// unconditional under both policies.
enum class LogosRestartPolicy {
    Automatic = 0,
    Manual,
};

#endif // LOGOS_SUBSCRIPTION_STATE_H
