#ifndef LOGOS_RPC_STATUS_H
#define LOGOS_RPC_STATUS_H

#include <QVariant>
#include <QVariantMap>
#include <QJsonObject>
#include <QString>
#include <QMetaType>

// A provider returns this reserved single-key map INSTEAD of a bare QVariant()
// when it rejects a call for an unrecognized/stale auth token. Backward
// compatible by construction:
//
//   * OLD consumers convert it exactly like a bare QVariant() for every
//     scalar / string / LogosResult return (`.toString()` -> "",
//     `qvariant_cast<T>` -> T{}, `.value<LogosResult>()` -> default), so they
//     keep seeing today's "empty/failed" result — nothing new to handle.
//   * NEW consumers (LogosAPIClient) detect it BELOW the generated typed
//     wrapper, drop the stale token, re-run capability_module.requestModule and
//     retry the call once. The sentinel is stripped before it ever reaches the
//     wrapper.
//
// The key is namespaced + double-underscored so it never collides with a real
// field, and the detector requires size()==1 with the exact key AND value, so a
// legitimate map result can't false-match. It is the ONLY provider->consumer
// signal available on every transport (qt_local/qt_remote/plain) without an ABI
// break, because the QtRO dispatch slot returns a single QVariant.
namespace logos {

inline constexpr char kRpcStatusKey[]          = "__logos_rpc_status__";
inline constexpr char kRpcStatusUnauthorized[] = "unauthorized";
// The token is valid but its grant does not cover the method: never re-exchanged.
inline constexpr char kRpcStatusNotAuthorised[] = "not_authorised";

// The provider-side rejection value. QVariantMap round-trips faithfully on all
// three transports (qt_local pass-through; qt_remote QtRO-serialized; plain via
// qvariant_rpc_value.cpp map<->JSON), so the consumer always sees a QVariantMap.
inline QVariant makeUnauthorizedSentinel()
{
    QVariantMap m;
    m.insert(QString::fromLatin1(kRpcStatusKey),
             QString::fromLatin1(kRpcStatusUnauthorized));
    return m;
}

inline QVariant makeNotAuthorisedSentinel()
{
    QVariantMap m;
    m.insert(QString::fromLatin1(kRpcStatusKey), QString::fromLatin1(kRpcStatusNotAuthorised));
    return m;
}

// True only for the exact status sentinel `status`. Handles QVariantMap (the normal
// case on every transport) and QJsonObject (defensive: some json_convert paths
// historically produced QJsonObject). Never matches a normal empty result.
inline bool isRpcStatusSentinel(const QVariant& v, const char* status)
{
    const QString key  = QString::fromLatin1(kRpcStatusKey);
    const QString want = QString::fromLatin1(status);
    switch (v.userType()) {
    case QMetaType::QVariantMap: {
        const QVariantMap m = v.toMap();
        return m.size() == 1 && m.value(key).toString() == want;
    }
    case QMetaType::QJsonObject: {
        const QJsonObject o = v.toJsonObject();
        return o.size() == 1 && o.value(key).toString() == want;
    }
    default:
        return false;
    }
}

inline bool isUnauthorizedSentinel(const QVariant& v)
{
    return isRpcStatusSentinel(v, kRpcStatusUnauthorized);
}

inline bool isNotAuthorisedSentinel(const QVariant& v)
{
    return isRpcStatusSentinel(v, kRpcStatusNotAuthorised);
}

} // namespace logos

#endif // LOGOS_RPC_STATUS_H
