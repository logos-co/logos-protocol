#ifndef LOGOS_ASYNC_DISPATCH_H
#define LOGOS_ASYNC_DISPATCH_H

#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantMap>

// Shared wire constants for "multi" (concurrent) dispatch. Concurrency is a
// MODULE-side concern handled entirely behind the ordinary callMethod entry
// point — no new provider/host vtable method, so the provider ABI is unchanged
// and an old host/daemon loads a "multi" module and forwards its traffic
// without even understanding these markers.
//
// A "multi" module's generated glue does NOT block in callMethod: it hands the
// handler to a worker and returns a PENDING SENTINEL immediately (a QVariantMap
// carrying the call id under pendingCallKey()). When the worker finishes, the
// module pushes the real result back as a COMPLETION event
// (callCompleteEvent(), data = [callId, result]) over the SAME event channel it
// already uses (setEventListener). The host (ModuleProxy / liblogos) is a pure
// forwarder — it returns whatever callMethod returned and forwards whatever
// events the module emits. The CONSUMER transport (RemoteLogosObject for QtRO,
// PlainLogosObject for the plain transport) detects the sentinel, waits for the
// matching completion keyed by callId, and returns the real result — so
// generated clients call transparently. Both transports use this path; the
// version that speaks it is logos-protocol 0.2 (additive minor — see
// logos_protocol.h).
namespace logos {

inline QString pendingCallKey()   { return QStringLiteral("__logos_pending_call__"); }
inline QString callCompleteEvent() { return QStringLiteral("__logos_call_complete__"); }

// True only for the exact pending-call sentinel, writing the call id to `callId`.
//
// The four detection sites used to test `m.contains(pendingCallKey())` with no
// shape check at all, so ANY user map that happened to carry that key was taken
// for a deferred call: the consumer then waited in a nested event loop for a
// completion that never arrives, and the call hung for the full timeout. An
// `any` slot is enough to reach it, i.e. anything a user can put in a map.
//
// The generated glue builds this map with exactly one entry whose value is a
// QString call id, so the checks below are behaviour-preserving for every real
// sender. Shape is mirrored from isUnauthorizedSentinel (logos_rpc_status.h),
// including the QJsonObject arm — the two are the same kind of in-band marker
// and there is no reason for them to be guarded differently.
//
// NARROWS, DOES NOT CLOSE. A forgery of the canonical shape — one key, string
// value — is still indistinguishable from the real thing, because that IS the
// real thing. Closing it needs an out-of-band channel for "deferred", which the
// single-QVariant dispatch slot cannot express without an ABI break.
inline bool isPendingCallSentinel(const QVariant& v, QString* callId = nullptr)
{
    const QString key = pendingCallKey();
    QString id;
    switch (v.userType()) {
    case QMetaType::QVariantMap: {
        const QVariantMap m = v.toMap();
        if (m.size() != 1) return false;
        const QVariant held = m.value(key);
        if (held.userType() != QMetaType::QString) return false;
        id = held.toString();
        break;
    }
    case QMetaType::QJsonObject: {
        const QJsonObject o = v.toJsonObject();
        if (o.size() != 1) return false;
        const QJsonValue held = o.value(key);
        if (!held.isString()) return false;
        id = held.toString();
        break;
    }
    default:
        return false;
    }
    if (id.isEmpty()) return false;
    if (callId) *callId = id;
    return true;
}

}  // namespace logos

#endif  // LOGOS_ASYNC_DISPATCH_H
