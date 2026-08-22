#ifndef MODULE_PROXY_H
#define MODULE_PROXY_H

#include <QObject>
#include <QVariant>
#include <QVariantList>
#include <QHash>
#include <QString>
#include <QJsonArray>
#include <QPointer>

#include <functional>
#include <string>
#include <utility>

#include "token_manager.h"

class LogosProviderObject;

namespace logos {

/**
 * @brief How many constant-time token comparisons this image has performed.
 *
 * INSTRUMENTATION, not a knob and not a diagnostic anyone should act on. It
 * exists so a test can assert the one timing property that is deterministic
 * enough to be worth asserting: the number of comparisons ModuleProxy performs
 * for an inbound call is a function of the STORE SIZES only — never of where
 * the matching token sits, nor of whether there was a match at all. An
 * accidental `break` or early `return` in either scan loop is exactly what that
 * catches, and is exactly what the constant-time compare exists to prevent.
 *
 * It asserts nothing about wall-clock time and no test here should claim it
 * does; see the note in tests/protocol/test_inbound_token_store.cpp.
 *
 * Always on rather than behind a build flag: a relaxed atomic increment is
 * unmeasurable next to the two heap allocations QString::toUtf8() already makes
 * on every one of these comparisons, and a check that only exists in a test
 * build is a check that stops matching the shipped code.
 */
unsigned long long tokenComparisonCount();

// The name a module's handshake surface is published under.
//
// A module's initializer is synchronous and routinely calls out — including
// capability_module's requestModule, which capability answers by pushing a
// token back to that same module. The module's BUSINESS object is published
// only after the initializer returns (so a caller keeps waiting at acquire
// until the module is genuinely ready, which is the long-standing contract).
// That left the push unsatisfiable: capability waited for a source that could
// not appear until the initializer returned, and the initializer could not
// return until capability answered.
//
// The handshake object is published BEFORE the initializer runs and carries
// token delivery only. capability can therefore always reach a module, while
// callers of real methods still block at acquire exactly as they always have.
inline QString handshakeObjectName(const QString& moduleName)
{
    return moduleName + QStringLiteral("__handshake");
}
} // namespace logos

/**
 * @brief ModuleProxy wraps a LogosProviderObject and exposes it as a QObject
 *        so that Qt Remote Objects can publish it.
 *
 * All method dispatch, introspection, and event forwarding is delegated
 * to the underlying LogosProviderObject*.  For legacy QObject-based plugins,
 * that provider is a QtProviderObject adapter; for new-API plugins it is
 * the plugin's own LogosProviderObject subclass.
 */
class ModuleProxy : public QObject
{
    Q_OBJECT

public:
    // A host-installed extra authorizer. Returns true if `token` is valid for a
    // call arriving over `transportProtocol` ("local" | "tcp" | "tcp_ssl").
    // Consulted IN ADDITION to the built-in issued-token scan, so installing one
    // only ever grants access to tokens the built-in scan wouldn't (e.g. the
    // daemon backs it with TokenStore::lookupByToken to make operator-issued
    // named tokens work, with per-token expiry and local_only enforced by the
    // transport it's handed).
    using TokenValidator = std::function<bool(const QString& token,
                                              const QString& transportProtocol)>;

    // `token_store` is the store this proxy AUTHORIZES AGAINST — the host
    // anchors plus whatever else the host seeded for this provider's identity.
    // It must be the same store the provider's own informModuleToken writes to,
    // which for the Qt stack is LogosAPI::getTokenManager() ==
    // TokenManager::forIdentity(<this module's name>), NOT the ambient
    // instance(). Those are the same object until a host isolates the identity;
    // after that they diverge and hardcoding instance() means two different
    // failures at once — every token the host seeded privately is invisible
    // (inbound calls rejected with no diagnostic), and every token in the
    // ambient ring is still accepted (the escalation isolation exists to close).
    //
    // Defaulted to &TokenManager::instance() so every existing two-argument
    // construction keeps scanning exactly what it scanned before. A null
    // pointer means the same thing.
    explicit ModuleProxy(LogosProviderObject* provider, QObject* parent = nullptr,
                         TokenManager* token_store = nullptr);
    ~ModuleProxy();

    void setTokenValidator(TokenValidator validator);

    // Two explicit Q_INVOKABLE overloads rather than one with a defaulted
    // transport arg: the Qt meta-object system matches by full parameter list
    // and does not apply C++ default arguments, so the existing QtRO/local
    // 3-arg call must remain a real 3-arg method. It forwards to the
    // transport-aware 4-arg form with "local" (RemoteTransportHost is always
    // local); remote hosts that know their wire (PlainTransportHost) call the
    // 4-arg form so a transport-sensitive validator (local_only tokens) can
    // enforce it.
    Q_INVOKABLE QVariant callRemoteMethod(const QString& authToken, const QString& methodName, const QVariantList& args = QVariantList());
    Q_INVOKABLE QVariant callRemoteMethod(const QString& authToken, const QString& methodName, const QVariantList& args, const QString& transportProtocol);
    Q_INVOKABLE bool informModuleToken(const QString& authToken, const QString& moduleName, const QString& token);
    bool saveToken(const QString& from_module_name, const QString& token);
    // getPluginInterface() returns the module's whole interface (methods AND
    // events, each tagged with a "type"); getPluginMethods()/getPluginEvents()
    // are the type-filtered views. All three derive from the provider's single
    // getMethods() call — there is no separate getEvents() vtable method, which
    // is what keeps the provider ABI stable across SDK versions.
    Q_INVOKABLE QJsonArray getPluginMethods();
    Q_INVOKABLE QJsonArray getPluginEvents();
    Q_INVOKABLE QJsonArray getPluginInterface();

signals:
    void eventResponse(const QString& eventName, const QVariantList& data);

private:
    // Returns true when authToken matches a token THIS module has been told about
    // (via saveToken / informModuleToken), or one held in this proxy's token
    // store, OR the host-installed validator accepts it for `transportProtocol`.
    // Empty/unknown tokens are rejected. The built-in comparison is constant-time
    // and never early-outs, so neither a correct prefix nor the number of issued
    // tokens leaks through timing.
    //
    // Kept as the two-argument spelling every existing call site and comment in
    // the fleet names; it forwards to authorize() below with no caller-out.
    bool isAuthorized(const QString& authToken, const QString& transportProtocol) const;

    // The same decision, PLUS who made it.
    //
    // Fused into one scan rather than added as a second pass, for two reasons
    // and the second is the important one. It costs zero extra comparisons:
    // deciding whether a presented token matches an issued one is already a walk
    // over every issued token, and the key is right there. And it keeps the
    // constant-time property in ONE place — a separate "now find the name" loop
    // is a second scan whose early-out looks obviously harmless and would
    // reintroduce, in three lines, exactly the leak constantTimeEquals exists to
    // close.
    //
    // On `true`, *callerJson (when non-null) receives the caller document
    // described in logos_caller_scope.h — always a valid document, never empty,
    // Unknown where the caller cannot be named honestly. Untouched on `false`
    // beyond the Unknown it is initialised to: an unauthorized call has no
    // caller because it has no dispatch.
    bool authorize(const QString& authToken, const QString& transportProtocol,
                   std::string* callerJson) const;

    LogosProviderObject* m_provider;
    // THE INBOUND STORE: caller name -> the token that caller may present to us.
    // Direction-pure by construction — the only writers are saveToken() and
    // informModuleToken(), both of which key by the CALLER — which is what makes
    // it the only store here that can honestly NAME a caller.
    //
    // Do not reverse-look-up m_store for that. TokenManager is direction-MIXED:
    // LogosAPIClient writes the token we will PRESENT to a callee under the
    // CALLEE's name (logos_api_client.cpp:176), while inbound tokens are written
    // under the CALLER's name. A hit there may name a module we CALL as the
    // module CALLING us, which is affirmatively wrong and worse than unknown.
    QHash<QString, QString> m_tokens;
    // Never null after construction; see the constructor comment.
    //
    // A new data member here is safe in a way one in LogosAPI is not (see the
    // warning at logos_api.h:329). Nothing hands a ModuleProxy across an image
    // boundary: it is constructed by the Qt host (LogosAPIProvider) and reached
    // only through QMetaObject dispatch or, from a module cdylib, not at all —
    // the type that crosses is the LogosProviderObject vtable, which is
    // untouched.
    TokenManager* m_store;
    TokenValidator m_validator;
};

/**
 * @brief The token-delivery-only surface described by logos::handshakeObjectName.
 *
 * Deliberately tiny: it exposes informModuleToken and nothing else, so
 * publishing it early cannot expose business methods on a module that has not
 * finished initializing. It forwards to the ModuleProxy that owns it, so a
 * token delivered here lands in exactly the same store the business object
 * consults later.
 */
class ModuleHandshakeProxy : public QObject
{
    Q_OBJECT

public:
    explicit ModuleHandshakeProxy(ModuleProxy* proxy, QObject* parent = nullptr);

    Q_INVOKABLE bool informModuleToken(const QString& authToken,
                                       const QString& moduleName,
                                       const QString& token);

private:
    QPointer<ModuleProxy> m_proxy;
};

#endif // MODULE_PROXY_H
