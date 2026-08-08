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

#endif // LOGOS_SUBSCRIPTION_STATE_H
