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
};

#endif // LOGOS_SUBSCRIPTION_STATE_H
