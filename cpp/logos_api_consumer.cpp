#include "logos_api_consumer.h"
#include "logos_object.h"
#include "module_proxy.h"
#include "token_manager.h"
#include "logos_mode.h"
#include "logos_instance.h"
#include "logos_transport.h"
#include "logos_transport_factory.h"
#include <chrono>
#include <thread>
#include <QDebug>
#include <QUrl>
#include <QMetaObject>
#include <QTimer>
#include <QTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QElapsedTimer>
#include <QSet>
#include <QVector>

// ── LogosPendingSubscriptions ────────────────────────────────────────────────
//
// The registry behind LogosAPIConsumer::onEventWhenAvailable(). It holds every
// subscription whose target object is not reachable yet and arms it the moment
// it becomes reachable — see the contract on the header declaration.
//
// Defined here rather than in the header on purpose: all of its state is
// instance state reached through one owned pointer, so nothing about it can
// become a per-image duplicate on Windows.
class LogosPendingSubscriptions
{
public:
    LogosPendingSubscriptions(LogosAPIConsumer* owner,
                              LogosTransportConnection* transport)
        : m_owner(owner), m_transport(transport) {}

    ~LogosPendingSubscriptions()
    {
        if (m_timer) { m_timer->stop(); delete m_timer; }
        for (LogosObject* obj : m_handles)
            if (obj) obj->release();
        m_handles.clear();
    }

    quint64 add(const QString& objectName, const QString& eventName,
                LogosObject::EventCallback cb, std::function<void(bool)> onArmed,
                bool readinessOnly = false)
    {
        Entry e;
        e.id            = m_nextId++;
        e.objectName    = objectName;
        e.eventName     = eventName;
        e.callback      = std::move(cb);
        e.onArmed       = std::move(onArmed);
        e.readinessOnly = readinessOnly;
        e.since.start();
        const quint64 id = e.id;
        m_entries.push_back(std::move(e));

        // If a handle for this object is already live, arm right now.
        if (LogosObject* obj = liveHandle(objectName)) {
            armAgainst(objectName, obj);
            return id;
        }
        if (beginAcquire(objectName))
            return id;                                 // armed synchronously

        qDebug().nospace() << "LogosAPIConsumer: '" << objectName << "::" << eventName
                           << "' deferred pending the module becoming reachable";
        ensureTimer();
        return id;
    }

    // Stop tracking a subscription. A PENDING one leaves the registry entirely
    // (it stops holding the timer up and stops the watchdog warning about a
    // subscription nobody wants any more); an ARMED one is dropped from the
    // re-arm set so a later reconnect does not resurrect it.
    //
    // Deliberately does NOT detach the callback from the handle: LogosObject
    // exposes no per-callback removal, only clearEventSubscriptions(), which
    // would take out every OTHER subscriber on the same shared handle. Callers
    // that must stop delivery (lp_unsubscribe) gate their own callback; this
    // just stops the bookkeeping from outliving them.
    bool cancel(quint64 id)
    {
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].id != id) continue;
            m_entries.remove(i);
            stopTimerIfIdle();
            return true;
        }
        for (int i = 0; i < m_armed.size(); ++i) {
            if (m_armed[i].id != id) continue;
            m_armed.remove(i);
            return true;
        }
        for (int i = 0; i < m_held.size(); ++i) {
            if (m_held[i].id != id) continue;
            m_held.remove(i);
            return true;
        }
        return false;
    }

    LogosSubscriptionState state(quint64 id) const
    {
        for (const Entry& e : m_entries)
            if (e.id == id) return LogosSubscriptionState::Pending;
        for (const Entry& e : m_armed)
            if (e.id == id) return LogosSubscriptionState::Armed;
        for (const Entry& e : m_held)
            if (e.id == id) return LogosSubscriptionState::Held;
        return LogosSubscriptionState::Unknown;
    }

    quint64 generation(const QString& objectName) const
    {
        return m_targets.value(objectName).generation;
    }

    void setStatusCallback(
        const QString& objectName,
        std::function<void(LogosSubscriptionEvent, quint64, const QString&)> cb)
    {
        Target& t = m_targets[objectName];
        t.onStatus = std::move(cb);

        // Replay where the object IS, so a watcher installed after a synchronous
        // arm does not wait for an edge that already happened. Copied out
        // first: the callback may re-enter and rehash m_targets.
        const auto fn   = t.onStatus;
        const quint64 g = t.generation;
        if (!fn) return;
        if (hasArmedFor(objectName))     fn(LogosSubscriptionEvent::Armed, g, QString());
        else if (hasHeldFor(objectName)) fn(LogosSubscriptionEvent::Held, g,
                                            QStringLiteral("provider_unavailable"));
    }

    // Set the restart policy for an OBJECT. Takes effect on the NEXT loss; it
    // never touches any subscription's current position, so switching to Manual
    // while armed is safe and switching to Automatic while HELD does not itself
    // revive anything (call rearm() for that — an implicit revive here would
    // make the setter's effect depend on state the caller cannot see).
    //
    // Cannot fail: the object need not have any subscriptions yet, which is the
    // point. A consumer sets the policy once when it creates its client and
    // never has to order that against its own first subscribe.
    void setRestartPolicy(const QString& objectName, LogosRestartPolicy policy)
    {
        m_targets[objectName].restart = policy;
    }

    // Move an object's held subscriptions back into the pending set and chase
    // it again. The generation is untouched: it advances on the ARM, so the
    // subscriber still sees Held(N) -> Armed(N+1) and can tell the gap
    // happened.
    bool rearm(const QString& objectName)
    {
        QVector<Entry> keep;
        bool any = false;
        for (Entry& e : m_held) {
            if (e.objectName != objectName) { keep.push_back(std::move(e)); continue; }
            e.since.start();
            e.warnLevel = 0;
            m_entries.push_back(std::move(e));
            any = true;
        }
        if (!any) return false;
        m_held = std::move(keep);

        if (LogosObject* obj = liveHandle(objectName))
            armAgainst(objectName, obj);   // provider already back
        else
            beginAcquire(objectName);
        ensureTimer();
        return true;
    }

    QStringList pending() const
    {
        QStringList out;
        for (const Entry& e : m_entries)
            out << (e.objectName + QStringLiteral("::")
                    + (e.readinessOnly ? QStringLiteral("(readiness)")
                       : e.eventName.isEmpty() ? QStringLiteral("(any)")
                       : e.eventName));
        return out;
    }

    // The connection was torn down and rebuilt (LogosAPIConsumer::reconnect):
    // every handle we hold points at a replica whose node is gone. Put the
    // armed subscriptions back into the pending set so they re-arm against the
    // new connection instead of going quietly dead.
    //
    // NOTE this is NOT the module-unload path. A module that unloads and comes
    // back drives its replica Suspect → Valid on the SAME node, and the event
    // helper is attached to that replica, so those subscriptions survive on
    // their own with nothing to do here.
    void reconnected()
    {
        for (LogosObject* obj : m_handles)
            if (obj) obj->release();
        m_handles.clear();
        m_acquiring.clear();

        QVector<Entry> revive = std::move(m_armed);
        m_armed.clear();
        QSet<QString> chase, holdReported;
        for (Entry& e : revive) {
            // Manual holds here too, deliberately UNIFORM with the liveness
            // watchdog: one rule, no exception to remember. The argument for
            // exempting reconnect is real — the provider never went away, only
            // our socket did — but a subscriber that asked to control its own
            // restarts should not have that decision made for it by which of
            // the two paths happened to notice, and the two are not otherwise
            // distinguishable from the outside.
            if (m_targets.value(e.objectName).restart == LogosRestartPolicy::Manual) {
                qWarning().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' subscription HELD (generation "
                    << m_targets.value(e.objectName).generation
                    << ") -- the connection was rebuilt and this module's restart "
                       "policy is Manual. Call rearmSubscriptions() to revive it.";
                holdReported.insert(e.objectName);
                m_held.push_back(std::move(e));
                continue;
            }
            e.since.start();
            e.warnLevel = 0;
            chase.insert(e.objectName);
            m_entries.push_back(std::move(e));
        }
        // One edge per OBJECT, after the moves: a status callback may re-enter
        // to rearm(), and it must find the held set already populated or the
        // revive it asks for is a silent no-op.
        for (const QString& name : holdReported)
            reportTarget(name, LogosSubscriptionEvent::Held,
                         QStringLiteral("connection_reset"));
        for (const QString& name : chase)
            beginAcquire(name);

        // MUST be last, and must happen even when beginAcquire() armed some of
        // them: takeMatching() stopped the timer when these subscriptions first
        // armed, and tick() is the sole driver of BOTH the retry and the only
        // log voice. Leaving it stopped is a subscription that is dead AND
        // silent -- a quieter version of the bug this class exists to remove.
        ensureTimer();
    }

private:
    struct Entry {
        quint64 id = 0;
        QString objectName;
        QString eventName;
        LogosObject::EventCallback callback;
        std::function<void(bool)> onArmed;
        // Per-ENTRY, unlike the generation number (see Target): a subscription
        // taken for the first time against an object that already restarted
        // twice is not itself re-arming.
        bool everArmed = false;
        QElapsedTimer since;
        int warnLevel = 0;   // 0 = quiet, 1 = warned at 3s, 2 = warned at 60s
        // Readiness-only: the caller wants to know WHEN the object becomes
        // acquirable, not to subscribe to anything. Fires onArmed exactly once
        // and is then forgotten — it is not re-armed on reconnect, because a
        // one-shot readiness answer that arrives twice is not an answer.
        bool readinessOnly = false;
    };

    // What is true of a TARGET rather than of one subscription to it: m_handles
    // holds one handle per object name, so losing a provider and regaining it
    // are indivisibly per-object. Created on demand and never erased — a policy
    // installed before the first subscribe must still be here when it arrives.
    struct Target {
        LogosRestartPolicy restart = LogosRestartPolicy::Automatic;
        // How many times this provider has been established. 0 until the first
        // arm, then +1 on each one.
        quint64 generation = 0;
        std::function<void(LogosSubscriptionEvent, quint64, const QString&)> onStatus;
    };

    // What the transport said when we asked it to acquire an object.
    enum class AcquireKind {
        Deferred,     // took ownership; the callback WILL fire. Never poll it.
        Declined,     // can defer, but not right now. Retry the deferred path.
        Unsupported,  // no deferred acquire at all. requestObject() is the only way.
    };

    LogosObject* liveHandle(const QString& objectName) const
    {
        LogosObject* obj = m_handles.value(objectName, nullptr);
        return (obj && obj->isValid()) ? obj : nullptr;
    }

    // Ask the transport to acquire `objectName`, without ever blocking.
    // Reports which of the three answers it gave; the caller decides how to
    // follow up. Does NOT poll and does NOT arm.
    AcquireKind startAcquire(const QString& objectName)
    {
        auto* async = dynamic_cast<LogosTransportAsyncAcquire*>(m_transport);
        if (!async) return AcquireKind::Unsupported;

        if (m_acquiring.contains(objectName))
            return AcquireKind::Deferred;               // one acquire per object

        QPointer<LogosAPIConsumer> guard(m_owner);
        const QString name = objectName;
        if (!async->requestObjectWhenAvailable(name, [this, guard, name](LogosObject* obj) {
                if (!guard) return;                     // consumer died first
                m_acquiring.remove(name);
                if (obj) armAgainst(name, obj);
                else     abandon(name);
            }))
            return AcquireKind::Declined;

        m_acquiring.insert(objectName);
        return AcquireKind::Deferred;
    }

    // startAcquire() plus the ONE synchronous attempt that transports without a
    // deferred acquire need. Returns true if the object was acquired and the
    // pending subscriptions for it are now armed.
    //
    // The synchronous attempt is not an optimisation, it is a correctness fix:
    // on qt_local/mock/plain the deferred path does not exist, so without it a
    // subscription to a module that is ALREADY loaded and in-process would not
    // arm until the first 250 ms tick, and every event emitted in that window
    // would be dropped. lp_subscribe used to attach synchronously and deliver
    // them; losing that would move the silent event loss rather than remove it.
    //
    // It is confined to AcquireKind::Unsupported on purpose. Those transports'
    // requestObject() is a registry hash lookup (qt_local), an in-memory
    // construction (plain) or an unconditional success (mock). qt_remote's
    // enters QRemoteObjectReplica::waitForSource()'s nested event loop even at
    // timeout 0, so calling it from here — or from tick() — would smuggle a GUI
    // thread block in through the retry. Routing on the transport's OWN answer
    // makes that structural instead of a comment someone has to remember.
    bool beginAcquire(const QString& objectName)
    {
        // Arm NOW if the transport can hand over a handle for free. Not an
        // optimisation — a correctness case the deferred path cannot cover.
        //
        // The common consumer shape is a successful CALL immediately followed
        // by a subscription in the same function (wallet-ui's backend calls
        // get_chains(), then subscribes on the next line). Before deferral, the
        // generated Qt wrapper acquired synchronously, so the subscription was
        // live before on() returned. Deferring it to the next event-loop turn
        // silently drops anything emitted in between, which is the same
        // event-loss this class exists to remove, just moved to a narrower
        // window. tryAcquireNow() never blocks and answers nullptr whenever it
        // would have to wait, so the deferred path below still owns every case
        // where the module is not already there.
        // Skipped while an acquire for this name is already in flight: that
        // PendingAcquire holds a replica and will arm every waiting entry at
        // once, so probing again buys nothing and only churns replicas. tick()
        // already applies this filter; this was the one caller that did not,
        // which is what turned one probe per module into one per subscription.
        if (auto* async = dynamic_cast<LogosTransportAsyncAcquire*>(m_transport);
            async && !m_acquiring.contains(objectName)) {
            if (LogosObject* now = async->tryAcquireNow(objectName)) {
                armAgainst(objectName, now);
                return true;
            }
        }

        if (startAcquire(objectName) != AcquireKind::Unsupported)
            return false;
        LogosObject* obj = m_transport->requestObject(objectName, 0);
        if (!obj) return false;
        armAgainst(objectName, obj);
        return true;
    }

    // One timer per consumer, running only while something is pending.
    //
    // On qt_remote it does NO polling at all — every pending object is in
    // m_acquiring, so tick() finds nothing to ask for and the timer exists
    // purely as the log watchdog below. On the other transports it also retries
    // requestObject(), at 250 ms → 5 s, costing a hash lookup or an in-memory
    // socket-state read per pending object per tick.
    //
    // Known cost, stated rather than hidden: on qt_local/mock a retry against a
    // module that is not registered makes the transport log its own "plugin not
    // found" warning, so a long-pending subscription there produces roughly one
    // such line per 5 s per object. That noise is deliberate — it is the
    // transport truthfully reporting a module that is not there — and silencing
    // it would be the silent-failure shape this whole change exists to remove.
    // The timer now has TWO jobs: chase pending subscriptions, and watch armed
    // ones for a provider that goes away. It runs while either set is
    // non-empty, where it used to run only for the first.
    bool timerNeeded() const { return !m_entries.isEmpty() || !m_armed.isEmpty(); }

    // Cadence for the armed-set liveness poll, deliberately faster than the
    // 5 s retry-backoff cap: this is the delay before a subscriber learns its
    // provider died, and it bounds how long a consumer keeps believing a dead
    // stream is merely quiet. One virtual isValid() per held handle per second
    // — on qt_remote a replica state read — is cheap enough that the tighter
    // number costs nothing worth measuring.
    static constexpr int kLivenessIntervalMs = 1000;

    void ensureTimer()
    {
        if (!timerNeeded()) return;
        if (!m_timer) {
            m_timer = new QTimer(m_owner);
            QObject::connect(m_timer, &QTimer::timeout, m_owner, [this]() { tick(); });
        }
        m_intervalMs = 250;                            // a new pending resets the backoff
        m_timer->start(m_intervalMs);
    }

    void tick()
    {
        // First, because a provider that just died turns armed subscriptions
        // back into pending ones that this same tick should then chase.
        checkLiveness();

        QSet<QString> wanted;
        for (const Entry& e : m_entries)
            if (!m_acquiring.contains(e.objectName)) wanted.insert(e.objectName);

        for (const QString& name : wanted) {
            // beginAcquire() re-asks the transport and only ever reaches
            // requestObject() on a transport that has no deferred acquire. A
            // transport that CAN defer but declined this round (qt_remote when
            // acquireDynamic() came back null) is simply retried on the
            // deferred path — polling it here is the nested-event-loop hazard.
            beginAcquire(name);
        }

        reportStillPending();
        rescheduleOrStop();
    }

    // 250 ms → 5 s cap. Unbounded in TIME on purpose: a module can be installed
    // and loaded mid-session, so any give-up would silently break the package
    // manager's core flow. What is bounded is the NOISE — two log lines per
    // (object, event), ever — and the timer itself, which stops once there is
    // nothing left for a tick to do.
    void rescheduleOrStop()
    {
        if (!m_timer) return;
        if (!timerNeeded()) { m_timer->stop(); return; }

        // Nothing PENDING to chase, but something is armed: fall back to the
        // liveness cadence. This is the new steady state — where the timer used
        // to stop once everything armed, it now idles doing one isValid() per
        // handle, which is what makes a provider's death observable at all.
        if (m_entries.isEmpty()) {
            m_intervalMs = kLivenessIntervalMs;
            m_timer->start(m_intervalMs);
            return;
        }

        // Nothing to do means: every pending object already has an acquire in
        // flight (the transport will arm it with no help from us) and every
        // pending entry has said everything it is ever going to say. That is
        // the steady state on qt_remote, where a tick does no work at all.
        // add() and reconnected() restart the timer if that changes.
        for (const Entry& e : m_entries) {
            if (!m_acquiring.contains(e.objectName) || e.warnLevel < 2) {
                if (m_intervalMs < 5000) {
                    m_intervalMs = qMin(5000, m_intervalMs * 2);
                    m_timer->start(m_intervalMs);
                }
                return;
            }
        }
        // Every pending entry is quiet and in flight. Keep ticking anyway if
        // anything is armed, so the watchdog does not go blind.
        if (!m_armed.isEmpty()) {
            m_intervalMs = kLivenessIntervalMs;
            m_timer->start(m_intervalMs);
            return;
        }
        m_timer->stop();
    }

    void stopTimerIfIdle()
    {
        if (m_timer && !timerNeeded()) m_timer->stop();
    }

    // Bounded diagnostics. A subscription that is deferred for a few
    // milliseconds during normal startup is not news and must not spam the log;
    // one that is still waiting seconds later is the shape of the original bug
    // and has to leave a durable record. So: one warning at 3 s, one more at
    // 60 s, then silence — never per retry.
    void reportStillPending()
    {
        for (Entry& e : m_entries) {
            const qint64 ms = e.since.elapsed();
            if (e.warnLevel == 0 && ms >= 3000) {
                e.warnLevel = 1;
                qWarning().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' still not reachable after " << ms
                    << " ms -- subscription is DEFERRED, not lost; it will arm when the "
                       "module appears. Is the module loaded?";
            } else if (e.warnLevel == 1 && ms >= 60000) {
                e.warnLevel = 2;
                qWarning().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' still pending after " << ms
                    << " ms. Still retrying; this is the last message about it.";
            }
        }
    }

    // A handle arrived: attach every pending subscription for that object.
    void armAgainst(const QString& objectName, LogosObject* obj)
    {
        LogosObject* handle = m_handles.value(objectName, nullptr);
        if (handle && handle != obj && !handle->isValid()) {
            // Releasing this handle destroys its event helper, and every
            // subscription already armed for this object is attached to THAT
            // helper. Without the revive they stay in m_armed, never fire
            // again, and pendingSubscriptions() reports nothing wrong — a
            // subscription that is dead while looking healthy, which is the
            // exact failure this class exists to remove. Move them back to the
            // pending set so takeMatching() below re-arms them on the new
            // handle, THEN release.
            //
            // NOT COVERED BY A TEST, deliberately, and it is worth knowing why
            // before anyone simplifies it away. LogosObject::isValid() defaults
            // to true and is overridden ONLY by qt_remote's RemoteLogosObject,
            // so on qt_local / mock / plain a held handle is always "live" and
            // add() short-circuits before a second acquire can start — this
            // branch is structurally unreachable there. On qt_remote it is
            // reachable but not reliably reproducible: QtRO shares one replica
            // implementation per object name on a node, so a reload usually
            // restores the old handle to Valid before the new acquire's
            // callback runs. The branch survives on an invariant of QtRO's that
            // nothing in this file controls; a test for it would be a race, and
            // a racing test is worse than none.
            reviveArmed(objectName);
            handle->release();
            handle = nullptr;
        }
        if (!handle) {
            m_handles.insert(objectName, obj);
            handle = obj;
        } else if (handle != obj) {
            obj->release();                            // already had a live one
        }

        // Split FIRST, run callbacks after. onArmed / onEvent can re-enter
        // add() (a consumer re-subscribing on arm), and mutating m_entries
        // while iterating it would be a use-after-free.
        QVector<Entry> matched = takeMatching(objectName);

        // The generation belongs to the OBJECT, so it advances once here, not
        // once per subscription. Readiness-only entries do not count: a
        // whenObjectAvailable() probe attaches nothing and must not look like a
        // re-establishment.
        bool anyReal = false;
        for (const Entry& e : matched)
            if (!e.readinessOnly) { anyReal = true; break; }
        if (anyReal) ++m_targets[objectName].generation;
        const quint64 gen = m_targets.value(objectName).generation;

        for (Entry& e : matched) {
            if (e.readinessOnly) {
                // No subscription to attach — the caller only wanted to know
                // the object had become acquirable. Answer once and forget it;
                // keeping it would re-answer on every reconnect.
                if (e.onArmed) e.onArmed(true);
                continue;
            }
            handle->onEvent(e.eventName, e.callback);
            // Re-arming is a per-entry question; the generation is the
            // object's. See Entry::everArmed.
            const bool reArm = e.everArmed;
            e.everArmed = true;
            // Log at the level that matches what was already said: if we
            // warned that this one was pending, close the loop out loud;
            // otherwise it armed promptly and is not news. A RE-arm is always
            // worth a line: it is the visible half of an unrecoverable gap.
            if (reArm)
                qInfo().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' subscription RE-ARMED (generation " << gen
                    << ") after " << e.since.elapsed()
                    << " ms -- events emitted while it was down were not delivered";
            else if (e.warnLevel > 0)
                qInfo().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' subscription ARMED after " << e.since.elapsed() << " ms";
            else
                qDebug().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << "' subscription armed after " << e.since.elapsed() << " ms";
            if (e.onArmed) e.onArmed(true);
            m_armed.push_back(std::move(e));           // keep, so reconnect can re-arm
        }

        // ONE edge for the object, and only after every entry is in m_armed: a
        // watcher may re-enter to read state or subscribe, and it must not see
        // half a transition.
        if (anyReal) reportTarget(objectName, LogosSubscriptionEvent::Armed, QString());

        // The armed set is now non-empty, so the liveness watchdog has work
        // even though nothing is pending. takeMatching() above stopped the
        // timer on exactly that condition; restart it.
        ensureTimer();
    }

    // Move every ARMED subscription for an object back into the pending set,
    // keeping its warnLevel so a re-arm produces no new noise.
    void reviveArmed(const QString& objectName)
    {
        QVector<Entry> keep;
        for (Entry& e : m_armed) {
            if (e.objectName == objectName) {
                e.since.start();
                m_entries.push_back(std::move(e));
            } else {
                keep.push_back(std::move(e));
            }
        }
        m_armed = std::move(keep);
    }

    // The mirror of reviveArmed for Manual subscriptions: park them in m_held
    // instead of putting them back in the pending set. Run this FIRST, so the
    // reviveArmed that follows finds only the Automatic ones.
    //
    // A SEPARATE VECTOR, not a flag on m_entries, and every reason is a sweep
    // that would otherwise pick a held entry back up:
    //   * takeMatching() is per-object and unconditional, so an unrelated
    //     subscriber arming this same object would re-arm the held one as a
    //     side effect — the bug that is hardest to see, because nothing the
    //     held subscription did caused it;
    //   * tick()'s `wanted` set would keep asking the transport for it;
    //   * reportStillPending() would warn that it is "DEFERRED, not lost" about
    //     something deliberately not being chased;
    //   * timerNeeded() would hold the watchdog up forever.
    // With m_held out of the pending set, all four are correct with no edits.
    void holdArmed(const QString& objectName)
    {
        const bool manual =
            m_targets.value(objectName).restart == LogosRestartPolicy::Manual;
        if (!manual) return;
        QVector<Entry> keep;
        for (Entry& e : m_armed) {
            if (e.objectName == objectName && manual)
                m_held.push_back(std::move(e));
            else
                keep.push_back(std::move(e));
        }
        m_armed = std::move(keep);
    }


    QVector<Entry> takeMatching(const QString& objectName)
    {
        QVector<Entry> matched, remaining;
        for (Entry& e : m_entries) {
            if (e.objectName == objectName) matched.push_back(std::move(e));
            else                            remaining.push_back(std::move(e));
        }
        m_entries = std::move(remaining);
        if (!timerNeeded() && m_timer) m_timer->stop();
        return matched;
    }

    // The transport proved this object can never be acquired on this
    // connection. Drop the subscriptions LOUDLY — a permanently dead
    // subscription that still looks pending is the original bug wearing a
    // different hat.
    void abandon(const QString& objectName)
    {
        const QVector<Entry> matched = takeMatching(objectName);
        for (const Entry& e : matched) {
            qWarning().nospace()
                << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                << "' ABANDONED -- the transport reported this object permanently "
                   "unavailable. This subscription will never fire.";
            if (e.onArmed) e.onArmed(false);
        }
        if (!matched.isEmpty())
            reportTarget(objectName, LogosSubscriptionEvent::Abandoned,
                         QStringLiteral("object_unreachable"));
    }

    // Liveness watchdog: the missing half of subscription continuity.
    //
    // A provider that unloads and reloads drives its replica out of Valid and
    // back on the SAME node, and the event helper stays attached to that
    // replica — so the subscription survives with nothing to do here and, until
    // now, nothing to SAY here either. reconnected() covers a torn-down
    // connection and reviveArmed() covers a handle being replaced; neither
    // observes a provider dying underneath a subscription that is already
    // armed. That is the case a subscriber cannot detect for itself, and it is
    // the one that silently loses events.
    //
    // So poll the handles we hold while anything is armed. LogosObject::isValid
    // is the transport's own answer (on qt_remote, the default, it is exactly
    // "the replica is still synced to its source"); the base returns true, so
    // on a transport with no notion of staleness this costs one virtual call
    // per handle per tick and never fires.
    void checkLiveness()
    {
        if (m_armed.isEmpty()) return;

        QSet<QString> dead;
        for (auto it = m_handles.cbegin(); it != m_handles.cend(); ++it)
            if (it.value() && !it.value()->isValid()) dead.insert(it.key());
        if (dead.isEmpty()) return;

        for (const QString& objectName : dead) {
            const bool held =
                m_targets.value(objectName).restart == LogosRestartPolicy::Manual;
            // A handle can go stale with nothing armed against it -- every
            // subscription to it cancelled, the handle not yet dropped. There
            // is no loss to report there: nobody was receiving anything.
            // Sampled BEFORE the moves below, which empty m_armed for this
            // object.
            const bool wasArmed = hasArmedFor(objectName);
            for (const Entry& e : m_armed) {
                if (e.objectName != objectName) continue;
                qWarning().nospace()
                    << "LogosAPIConsumer: '" << e.objectName << "::" << e.eventName
                    << (held ? "' subscription HELD (generation " : "' subscription LOST (generation ")
                    << m_targets.value(objectName).generation
                    << (held ? ") -- the provider became unreachable and this module's "
                               "restart policy is Manual, so it will NOT re-arm. Call "
                               "rearmSubscriptions() to revive it."
                             : ") -- the provider became unreachable. Re-arming; events "
                               "emitted before it returns are unrecoverable.");
            }

            // Drop the dead handle so liveHandle() stops answering with it and
            // beginAcquire() is free to fetch a fresh one.
            if (LogosObject* obj = m_handles.take(objectName))
                obj->release();
            m_acquiring.remove(objectName);
            // Park the Manual ones FIRST, so reviveArmed sees only Automatic.
            holdArmed(objectName);
            reviveArmed(objectName);

            // After the moves, so a watcher that calls rearmSubscriptions() on
            // Held finds the held set populated; before the chase, because
            // beginAcquire() can arm synchronously and would otherwise deliver
            // Armed ahead of the Lost it answers.
            if (wasArmed)
                reportTarget(objectName, held ? LogosSubscriptionEvent::Held
                                              : LogosSubscriptionEvent::Lost,
                             QStringLiteral("provider_unavailable"));

            // Only chase the object if something still wants it. An
            // all-Manual object has nothing pending, and asking the transport
            // for it would re-acquire a handle no subscription is waiting on.
            if (hasPendingFor(objectName))
                beginAcquire(objectName);
        }
        ensureTimer();
    }

    bool hasPendingFor(const QString& objectName) const
    {
        for (const Entry& e : m_entries)
            if (e.objectName == objectName) return true;
        return false;
    }

    bool hasArmedFor(const QString& objectName) const
    {
        for (const Entry& e : m_armed)
            if (e.objectName == objectName) return true;
        return false;
    }

    bool hasHeldFor(const QString& objectName) const
    {
        for (const Entry& e : m_held)
            if (e.objectName == objectName) return true;
        return false;
    }

    // Deliver one object-level edge. The callback is copied out first: a
    // watcher may re-enter and rehash m_targets, dangling the record.
    void reportTarget(const QString& objectName, LogosSubscriptionEvent ev,
                      const QString& reason)
    {
        auto it = m_targets.constFind(objectName);
        if (it == m_targets.cend() || !it->onStatus) return;
        const auto fn   = it->onStatus;
        const quint64 g = it->generation;
        fn(ev, g, reason);
    }

    LogosAPIConsumer* m_owner;
    LogosTransportConnection* m_transport;
    QVector<Entry> m_entries;   // waiting to arm
    QVector<Entry> m_armed;     // live; retained only so reconnected() can re-arm
    // Armed, then lost, under a Manual restart policy. Deliberately outside
    // m_entries so no sweep resurrects it — see holdArmed().
    QVector<Entry> m_held;
    QSet<QString> m_acquiring;
    // Per-TARGET policy, generation and watcher. Keyed by object name, created
    // on demand by the setters so a consumer can configure a module before it
    // subscribes to anything on it.
    QHash<QString, Target> m_targets;
    // Subscription handles, one per object, deliberately SEPARATE from
    // LogosAPIConsumer::m_objectCache. Sharing that cache would let the call
    // path release() a handle a live subscription is attached to (it drops a
    // stale entry on the next call), killing the subscription with no trace.
    QHash<QString, LogosObject*> m_handles;
    QTimer* m_timer = nullptr;
    int m_intervalMs = 250;
    quint64 m_nextId = 1;
};

LogosAPIConsumer::LogosAPIConsumer(const QString& module_to_talk_to,
                                   const QString& origin_module,
                                   TokenManager* token_manager,
                                   const LogosTransportConfig& transport,
                                   QObject *parent)
    : QObject(parent)
    , m_registryUrl(LogosInstance::id(module_to_talk_to))
    // Same NULL-means-the-origin's-store rule as LogosAPIClient, so a consumer
    // built directly (rather than through a client) carries the identity's store
    // too. m_token_manager is currently never READ at this layer — the store is
    // consulted only by LogosAPIClient — but it is a public constructor
    // parameter, so leaving it as the one place a null slips through would be a
    // trap for whoever does start reading it.
    , m_token_manager(token_manager ? token_manager
                                    : &TokenManager::forIdentity(origin_module))
{
    // Single transport-resolution path: the factory combines LogosMode
    // + LogosTransportConfig (mode wins for Mock/Local; transport
    // chooses the wire protocol in Remote mode). The choice scopes to
    // this consumer only — any LogosAPIProvider in the same LogosAPI
    // still constructs its host from the global default.
    m_transport = LogosTransportFactory::createConnection(transport, m_registryUrl);

    // Initial connect with deadline-driven retry. The target module's
    // listener may not be ready yet — particularly for TCP/TLS, where
    // the child subprocess's QTcpServer::listen() lags the runtime
    // returning from its load callback. QLocalSocket internally
    // tolerates this (it retries connect until a deadline), but
    // boost::asio::connect on TCP fails fast with "connection refused"
    // and we'd surface a warning + return nullptr for any subsequent
    // requestObject before the listener even came up.
    //
    // 50ms × up-to-100 attempts ≈ 5s budget — same shape as
    // logos-liblogos's sendTokenToProcess loop and generous enough to
    // cover cold-start child Qt initialisation under load.
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(5000);
    while (true) {
        if (m_transport->connectToHost()) break;
        if (clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

LogosAPIConsumer::LogosAPIConsumer(const QString& module_to_talk_to,
                                   const QString& origin_module,
                                   TokenManager* token_manager,
                                   QObject *parent)
    : LogosAPIConsumer(module_to_talk_to, origin_module, token_manager,
                       LogosTransportConfigGlobal::getDefault(), parent)
{
}

LogosAPIConsumer::~LogosAPIConsumer()
{
    // Release cached handles while m_transport is still alive (the destructor
    // body runs before member destruction). Same for the subscription registry,
    // which owns handles of its own.
    delete m_pendingSubs;
    m_pendingSubs = nullptr;
    clearObjectCache();
}

quint64 LogosAPIConsumer::onEventWhenAvailable(const QString& objectName,
                                               const QString& eventName,
                                               std::function<void(const QString&, const QVariantList&)> callback,
                                               std::function<void(bool)> onArmed)
{
    // An EMPTY eventName is deliberately allowed: LogosObject::onEvent reads it
    // as "every event on this object", and the arm path below hands eventName
    // straight to it, so the wildcard costs nothing to support and behaves
    // exactly as it does on the plain onEvent(). It used to be lumped in with
    // the two arguments that really are unusable, which silently denied every
    // hand-rolled wildcard subscriber the deferred path -- logoscore's
    // `watch <module>` with no --event is one.
    if (objectName.isEmpty() || !callback) {
        qWarning() << "LogosAPIConsumer::onEventWhenAvailable: empty object name "
                      "or null callback -- refusing" << objectName << eventName;
        if (onArmed) onArmed(false);
        return 0;
    }
    ensureRegistry();
    return m_pendingSubs->add(objectName, eventName, std::move(callback), std::move(onArmed));
}

bool LogosAPIConsumer::cancelEventSubscription(quint64 subscriptionId)
{
    if (!subscriptionId || !m_pendingSubs) return false;
    return m_pendingSubs->cancel(subscriptionId);
}

LogosSubscriptionState LogosAPIConsumer::eventSubscriptionState(quint64 subscriptionId) const
{
    if (!subscriptionId || !m_pendingSubs) return LogosSubscriptionState::Unknown;
    return m_pendingSubs->state(subscriptionId);
}

void LogosAPIConsumer::ensureRegistry()
{
    if (!m_pendingSubs)
        m_pendingSubs = new LogosPendingSubscriptions(this, m_transport.get());
}

void LogosAPIConsumer::setSubscriptionStatusCallback(
    const QString& objectName,
    std::function<void(LogosSubscriptionEvent, quint64, const QString&)> onStatus)
{
    if (objectName.isEmpty()) return;
    // CREATES the registry rather than bailing: it is built lazily by
    // onEventWhenAvailable(), so requiring it here would make
    // configure-then-subscribe a silent no-op.
    ensureRegistry();
    m_pendingSubs->setStatusCallback(objectName, std::move(onStatus));
}

quint64 LogosAPIConsumer::subscriptionGeneration(const QString& objectName) const
{
    if (objectName.isEmpty() || !m_pendingSubs) return 0;
    return m_pendingSubs->generation(objectName);
}

void LogosAPIConsumer::setSubscriptionRestartPolicy(const QString& objectName,
                                                    LogosRestartPolicy policy)
{
    if (objectName.isEmpty()) return;
    ensureRegistry();   // settable before the first subscribe — see above
    m_pendingSubs->setRestartPolicy(objectName, policy);
}

bool LogosAPIConsumer::rearmSubscriptions(const QString& objectName)
{
    if (objectName.isEmpty() || !m_pendingSubs) return false;
    return m_pendingSubs->rearm(objectName);
}

quint64 LogosAPIConsumer::whenObjectAvailable(const QString& objectName,
                                              std::function<void(bool)> onReady)
{
    if (objectName.isEmpty() || !onReady) {
        qWarning() << "LogosAPIConsumer::whenObjectAvailable: empty object name or null "
                      "callback -- refusing" << objectName;
        if (onReady) onReady(false);
        return 0;
    }
    ensureRegistry();
    return m_pendingSubs->add(objectName, QString(), {}, std::move(onReady),
                              /*readinessOnly=*/true);
}

QStringList LogosAPIConsumer::pendingSubscriptions() const
{
    return m_pendingSubs ? m_pendingSubs->pending() : QStringList();
}

LogosObject* LogosAPIConsumer::requestObject(const QString& objectName, Timeout timeout)
{
    qDebug() << "LogosAPIConsumer: Requesting object:" << objectName << "at" << QTime::currentTime().toString("hh:mm:ss.zzz");

    if (objectName.isEmpty()) {
        qWarning() << "LogosAPIConsumer: Object name cannot be empty";
        return nullptr;
    }

    if (!m_transport->isConnected()) {
        qWarning() << "LogosAPIConsumer: Not connected to registry. Cannot request object:" << objectName;
        return nullptr;
    }

    LogosObject* object = m_transport->requestObject(objectName, timeout.ms);
    if (object) {
        qDebug() << "[LogosObject] LogosAPIConsumer: acquired LogosObject for:" << objectName << "(id:" << object->id() << ")";
    }
    return object;
}

bool LogosAPIConsumer::isConnected() const
{
    return m_transport->isConnected();
}

QString LogosAPIConsumer::registryUrl() const
{
    return m_registryUrl;
}

bool LogosAPIConsumer::reconnect()
{
    qDebug() << "LogosAPIConsumer: Attempting to reconnect to registry:" << m_registryUrl;
    // Handles from the old connection point at replicas that are now dead; drop
    // them so the next call re-acquires against the fresh connection.
    clearObjectCache();
    const bool ok = m_transport->reconnect();
    // Same problem, different owner: deferred subscriptions hold their own
    // handles. Re-arm them rather than leaving them attached to dead replicas.
    if (m_pendingSubs) m_pendingSubs->reconnected();
    return ok;
}

TargetPresence LogosAPIConsumer::targetPresence(const QString& objectName)
{
    if (!m_transport) return TargetPresence::Unknown;
    // A cached handle proves the target published at least once on this
    // connection, which is the one thing every transport can agree on --
    // including plain, whose requestObject cannot answer presence at all.
    if (m_objectCache.contains(objectName)) return TargetPresence::Present;
    if (auto* presence = dynamic_cast<LogosTransportPresence*>(m_transport.get()))
        return presence->targetPresence(objectName);
    return TargetPresence::Unknown;
}

QVariant LogosAPIConsumer::invokeRemoteMethod(const QString& authToken, const QString& objectName, const QString& methodName,
                                   const QVariantList& args, Timeout timeout)
{
    return invokeRemoteMethod(authToken, objectName, methodName, args, timeout, nullptr);
}

QVariant LogosAPIConsumer::invokeRemoteMethod(const QString& authToken, const QString& objectName, const QString& methodName,
                                   const QVariantList& args, Timeout timeout, logos::CallError* err)
{
    if (err) err->clear();
    qDebug() << "LogosAPIConsumer: Calling invokeRemoteMethod:" << objectName << methodName << "args_count:" << args.size() << "timeout:" << timeout.ms;

    // Reuse a cached handle across calls. Acquiring a QtRO replica per call
    // (acquireDynamic + waitForSource) is expensive — under a tight loop (e.g. a
    // proxy forwarding every method to its target) it dominates and can starve
    // the nested synchronous calls. The handle is kept alive in m_objectCache
    // and re-acquired only when it goes stale.
    LogosObject* plugin = acquireCachedObject(objectName, timeout.ms);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object:" << objectName;
        if (err) {
            err->code = "object_unavailable";
            err->message = "failed to acquire remote object '"
                           + objectName.toStdString()
                           + "' (module not loaded, not published, or transport failure)";
            err->origin = objectName.toStdString();
        }
        return QVariant();
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: calling via LogosObject::callMethod" << methodName;
    // No release() here: the handle stays cached for the next call. Released in
    // clearObjectCache() (destructor / reconnect) or evicted when stale.
    //
    // Prefer the error channel when the transport implements it (see
    // LogosObjectErrorChannel in logos_object.h). Without it, `err` could only
    // ever describe an ACQUIRE failure — everything that went wrong after the
    // handle existed (the deadline elapsing, the connection dropping, the peer
    // answering "not published") came back as a bare QVariant() with a clean
    // err, i.e. reported as a method that returned null.
    if (auto* channel = dynamic_cast<LogosObjectErrorChannel*>(plugin))
        return channel->callMethodWithError(authToken, methodName, args,
                                            timeout.ms, err);
    return plugin->callMethod(authToken, methodName, args, timeout.ms);
}

// Get-or-acquire a remote-object handle, transparently refreshing a stale one.
LogosObject* LogosAPIConsumer::acquireCachedObject(const QString& objectName, int timeoutMs)
{
    if (LogosObject* cached = m_objectCache.value(objectName, nullptr)) {
        if (cached->isValid())
            return cached;
        // The source went away (module unloaded / transport dropped) — discard
        // the dead handle and acquire a fresh one below.
        qDebug() << "LogosAPIConsumer: cached handle for" << objectName << "went stale; re-acquiring";
        cached->release();
        m_objectCache.remove(objectName);
    }
    LogosObject* obj = m_transport->requestObject(objectName, timeoutMs);
    if (obj)
        m_objectCache.insert(objectName, obj);
    return obj;
}

void LogosAPIConsumer::clearObjectCache()
{
    for (LogosObject* obj : m_objectCache)
        if (obj) obj->release();
    m_objectCache.clear();
    // Drop the remembered absences too: after a reconnect, or once a module is
    // reloaded from a build that has the surface, it deserves a fresh probe.
    m_noHandshakeSurface.clear();
}

void LogosAPIConsumer::invokeRemoteMethodAsync(const QString& authToken, const QString& objectName, const QString& methodName,
                                                const QVariantList& args,
                                                AsyncResultCallback callback,
                                                Timeout timeout)
{
    // Delegate to the CallError-aware overload so there is one acquire/dispatch
    // path — the legacy callback simply drops the error field.
    invokeRemoteMethodAsync(authToken, objectName, methodName, args,
        [cb = std::move(callback)](QVariant r, const logos::CallError&) mutable {
            if (cb) cb(std::move(r));
        },
        timeout);
}

void LogosAPIConsumer::invokeRemoteMethodAsync(const QString& authToken, const QString& objectName, const QString& methodName,
                                                const QVariantList& args,
                                                AsyncResultErrorCallback callback,
                                                Timeout timeout)
{
    if (!callback) {
        qWarning() << "LogosAPIConsumer: invokeRemoteMethodAsync called with null callback";
        return;
    }

    // Reuse the cached handle, same as the sync path — repeated async calls to
    // one object (e.g. a proxy forwarding asynchronously) no longer re-acquire a
    // replica per call. The handle stays owned by m_objectCache; the callback
    // must NOT release it (it is shared across in-flight calls and freed only on
    // eviction/teardown, via release()'s deferred deleteLater).
    LogosObject* plugin = acquireCachedObject(objectName, timeout.ms);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object:" << objectName;
        logos::CallError err;
        err.code = "object_unavailable";
        err.message = "failed to acquire remote object '"
                      + objectName.toStdString()
                      + "' (module not loaded, not published, or transport failure)";
        err.origin = objectName.toStdString();
        QTimer::singleShot(0, this, [callback, err]() { callback(QVariant(), err); });
        return;
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: async calling via LogosObject::callMethodAsync" << methodName;
    // QPointer guards against use-after-free: if the consumer is destroyed
    // before the transport callback fires, the callback is silently dropped and
    // the handle is released by the destructor's clearObjectCache(), not here.
    QPointer<LogosAPIConsumer> self(this);

    // Prefer the error channel when the transport implements it. The lambda
    // below used to take only `QVariant result` and hand the caller a
    // hard-coded empty logos::CallError — so once acquire had succeeded, every
    // async outcome was reported as a success, whatever actually happened.
    if (auto* channel = dynamic_cast<LogosObjectErrorChannel*>(plugin)) {
        channel->callMethodAsyncWithError(authToken, methodName, args, timeout.ms,
            [callback, self](QVariant result, const logos::CallError& err) {
                if (!self)
                    return;
                callback(std::move(result), err);
            });
        return;
    }

    // Transport without an error channel (the mock): unchanged behaviour —
    // the value, and no diagnosis to give.
    plugin->callMethodAsync(authToken, methodName, args, timeout.ms,
        [callback, self](QVariant result) {
            if (!self)
                return;
            callback(result, logos::CallError{});
        });
}

void LogosAPIConsumer::onEvent(LogosObject* originObject, const QString& eventName, std::function<void(const QString&, const QVariantList&)> callback)
{
    qDebug() << "[LogosObject] LogosAPIConsumer::onEvent registering for:" << eventName << "on LogosObject id:" << originObject;

    if (!originObject) {
        qWarning() << "LogosAPIConsumer: Cannot register event on null object";
        return;
    }

    originObject->onEvent(eventName, std::move(callback));

    qDebug() << "[LogosObject] LogosAPIConsumer: event callback registered for:" << eventName;
}

bool LogosAPIConsumer::informModuleToken(const QString& authToken, const QString& moduleName, const QString& token)
{
    qDebug() << "LogosAPIConsumer: Informing module token for module:" << moduleName << "with token:" << redactToken(token);

    LogosObject* plugin = m_transport->requestObject("capability_module", 20000);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object: capability_module";
        return false;
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: calling LogosObject::informModuleToken for" << moduleName;
    bool result = plugin->informModuleToken(authToken, moduleName, token, 20000);
    qDebug() << "LogosAPIConsumer: informModuleToken completed with result:" << result;
    plugin->release();
    return result;
}

namespace {
// How long to wait when probing for a handshake surface before concluding the
// target predates it. Long enough to cover a live local socket round trip,
// short enough that the fallback is not perceptibly delayed.
constexpr int kHandshakeProbeTimeoutMs = 250;
} // namespace

bool LogosAPIConsumer::informModuleToken_module(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs)
{
    // A non-positive budget would make the wait transport-dependent rather than
    // bounded; fall back to the historical default.
    if (timeoutMs <= 0) {
        timeoutMs = 20000;
    }
    qDebug() << "LogosAPIConsumer: Informing module token for module:" << moduleName << "with token:" << redactToken(token);

    // Prefer the handshake surface. It is published before the target's
    // initializer runs, so it is reachable even while the target is still
    // starting up — which is the one case the business object cannot cover,
    // because that one is published only once the initializer returns.
    //
    // Short budget on this attempt: a module built before the handshake surface
    // existed simply has no such object, and we must not spend the full timeout
    // discovering that before falling back.
    // acquireCachedObject caches successes only, so without the negative cache
    // below a module built before this surface existed would pay the full probe
    // budget on EVERY grant — on QtRO that is a blocking waitForSource, i.e.
    // kHandshakeProbeTimeoutMs of dead time per token, forever. Remember the
    // absence instead and go straight to the business object. Cleared with the
    // handle cache on reconnect/destroy, so a module that comes back with a
    // handshake surface is re-probed rather than written off permanently.
    const QString handshake = logos::handshakeObjectName(originModule);
    if (m_noHandshakeSurface.contains(handshake)) {
        return informModuleTokenViaBusinessObject(authToken, originModule, moduleName, token, timeoutMs);
    }
    LogosObject* early = acquireCachedObject(handshake, kHandshakeProbeTimeoutMs);
    if (!early) {
        m_noHandshakeSurface.insert(handshake);
        qDebug() << "LogosAPIConsumer:" << originModule << "publishes no handshake surface"
                 << "- not probing again until the handle cache is cleared";
    }
    if (early) {
        qDebug() << "[LogosObject] LogosAPIConsumer: delivering token for" << moduleName
                 << "via the handshake surface of" << originModule;
        if (early->informModuleToken(authToken, moduleName, token, timeoutMs)) {
            qDebug() << "LogosAPIConsumer: informModuleToken completed with result: true";
            return true;
        }
        // A refusal HERE is not authoritative, so do not report it as the answer.
        // The handshake surface goes live before the target's initializer runs,
        // and a target whose token store is only seeded by that initializer will
        // refuse a push that arrives first. Falling through to the business
        // object — which exists only once the initializer has returned, by which
        // point the store is populated — is what the caller got before this
        // surface existed. Returning false here instead would hand the caller an
        // empty grant that it has no way to distinguish from a real denial.
        //
        // The wait below is bounded by the caller's own budget. Note what that
        // is worth in the path that matters: capability_module reaches here
        // through the FOUR-argument informModuleToken_module
        // (capability_module_plugin.cpp:112), so timeoutMs takes this header's
        // 20 s default. The budget is real, it is just not short.
        qWarning() << "LogosAPIConsumer: handshake surface of" << originModule
                   << "refused the token for" << moduleName
                   << "- it is probably still initializing; retrying on the business object";
    }

    return informModuleTokenViaBusinessObject(authToken, originModule, moduleName, token, timeoutMs);
}

// Fall back to the business object: modules built before the handshake surface
// existed are reached exactly as they always were. Also the landing place for a
// handshake surface that refused the push (target still initializing).
bool LogosAPIConsumer::informModuleTokenViaBusinessObject(const QString& authToken, const QString& originModule, const QString& moduleName, const QString& token, int timeoutMs)
{
    LogosObject* plugin = acquireCachedObject(originModule, timeoutMs);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object:" << originModule
                   << "- no handshake surface and no published business object"
                   << "(waited" << timeoutMs << "ms; it may still be initializing)";
        return false;
    }

    qDebug() << "[LogosObject] LogosAPIConsumer: calling LogosObject::informModuleToken for" << moduleName << "on" << originModule;
    bool result = plugin->informModuleToken(authToken, moduleName, token, timeoutMs);
    qDebug() << "LogosAPIConsumer: informModuleToken completed with result:" << result;
    // The cache owns the handle now, so it is not released here.
    return result;
}

// timeoutMs bounds the WHOLE handshake — the capability_module acquire plus the
// requestModule call on it — not each half. Two hardcoded 20 s waits used to
// live here, which is why a caller that asked for a short budget did not get
// one: LogosAPIClient::invokeRemoteMethod takes a Timeout, but the token
// exchange that runs FIRST on an un-tokened target ignored it, so a call
// advertising a 1500 ms bound could block on the order of 40 s before the
// bounded part even started. A budget that only covers the second half of an
// operation is not a budget.
//
// The acquire and the call therefore share one deadline rather than getting one
// each. Halving it would be arbitrary, and giving each the full amount would
// make the worst case twice what the caller asked for.
std::string LogosAPIConsumer::requestModule(const std::string& authToken, const std::string& originModule, const std::string& targetModule, int timeoutMs)
{
    const QString qOrigin = QString::fromStdString(originModule);
    const QString qTarget = QString::fromStdString(targetModule);
    qDebug() << "LogosAPIConsumer: requestModule for origin:" << qOrigin << "target:" << qTarget
             << "budget:" << timeoutMs << "ms";

    QElapsedTimer budget;
    budget.start();

    LogosObject* plugin = m_transport->requestObject("capability_module", timeoutMs);
    if (!plugin) {
        qWarning() << "LogosAPIConsumer: Failed to acquire plugin/replica for object: capability_module"
                   << "after" << budget.elapsed() << "ms of a" << timeoutMs << "ms budget";
        return {};
    }

    // What is left of the budget. Never 0: a transport reads 0 as "no timeout"
    // on some paths, so an exhausted budget must ask for the smallest real wait
    // rather than accidentally asking for an unbounded one.
    const qint64 spent = budget.elapsed();
    const int remaining = timeoutMs <= 0
        ? timeoutMs
        : static_cast<int>(qMax<qint64>(1, timeoutMs - spent));

    QVariant result = plugin->callMethod(QString::fromStdString(authToken), QStringLiteral("requestModule"),
                                         QVariantList() << qOrigin << qTarget, remaining);
    plugin->release();
    return result.toString().toStdString();
}
