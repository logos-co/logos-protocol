#ifndef TOKEN_MANAGER_H
#define TOKEN_MANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QMutex>
#include <QByteArray>
#include <QCryptographicHash>
#include <string>
#include <vector>

#include "logos_shared_api.h"

/**
 * @brief Render a capability/auth token safe to write to logs.
 *
 * Tokens gate all cross-module RPC and are accepted by value (see
 * ModuleProxy::isAuthorized), so a raw token recovered from a log line is
 * directly replayable. This collapses a token to a non-reversible, non-
 * replayable fingerprint — a fixed prefix plus the first bytes of its SHA-256 —
 * suitable for correlating log lines without exposing the secret. The
 * "redacted:" prefix signals to anyone reading the log that this is a
 * deliberately non-replayable fingerprint, not a truncated real token. Empty
 * tokens render as "<none>" so missing-token cases stay greppable.
 *
 * Always pass tokens through this before logging; never log the raw value.
 */
inline QString redactToken(const QString& token)
{
    if (token.isEmpty()) {
        return QStringLiteral("<none>");
    }
    const QByteArray digest =
        QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
    return QStringLiteral("redacted:") + QString::fromLatin1(digest.toHex().left(8)) + QStringLiteral("…");
}

/**
 * @brief TokenManager provides a singleton interface for managing authentication tokens
 * 
 * This class manages a collection of tokens identified by keys, providing thread-safe
 * access to store, retrieve, and manage tokens throughout the application lifecycle.
 *
 * LOGOS_SHARED_API: the singleton below is a function-local static, so it is one
 * per copy of the code. On PE that means one per IMAGE unless the consumer
 * imports it from liblogos_core.dll — which is exactly the token-invisibility
 * bug described in logos_shared_api.h. Off Windows, and inside the provider,
 * this expands to nothing.
 */
class LOGOS_SHARED_API TokenManager : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Get the singleton instance of TokenManager
     * @return TokenManager& Reference to the singleton instance
     */
    static TokenManager& instance();

    /* ── per-identity token stores ──────────────────────────────────────────
     *
     * WHAT THIS EXISTS TO CLOSE. instance() is the IMAGE's store, and in a host
     * that loads plugins in-process it is also an ambient ring: the host writes
     * `name -> that module's root auth token` for EVERY loaded module. On the
     * hot path a client asserts no identity at all — LogosAPIClient::
     * invokeRemoteMethod reads the store first and only mints on a miss — so
     * whichever plugin asks for target X finds X's own root token sitting there
     * and presents it. The provider accepts any token in its image's store, so
     * the call authorizes and no `requestModule` is ever logged. Every plugin in
     * that image therefore holds every other module's authority, and giving a
     * plugin its own ORIGIN STRING does not change that by one byte, because
     * origin is never consulted on the path taken.
     *
     * The fix is to make origin SELECT THE STORE rather than merely label the
     * caller. forIdentity(x) is "the store to present tokens from when I am x".
     *
     * ADDITIVE BY CONSTRUCTION, and that is not a promise but the shape of the
     * code: forIdentity() returns instance() — the same object, pointer-identical
     * — for every identity until someone calls isolateIdentity() on that exact
     * name. A host that knows nothing about any of this sees no change at all.
     * These are static member FUNCTIONS: no data member, no virtual, nothing moc
     * sees, so the layout every statically-linked plugin was compiled against is
     * untouched.
     *
     * PER-IMAGE IS NOT PER-CLIENT, and both are needed. A module cdylib links
     * its own copy of this library and therefore has its own instance(); that
     * per-image store is correct and stays. This adds a second axis INSIDE one
     * image, for the case the per-image split cannot reach: several identities
     * hosted by one process.
     */

    /**
     * @brief The token store for calling-identity `identity`.
     *
     * Returns instance() unless `identity` has been isolated, in which case it
     * returns that identity's private store. The address is stable for the
     * lifetime of the image, so a client may hold it by raw pointer (which
     * LogosAPIClient does, and dereferences from async continuations that can
     * outlive their caller).
     *
     * Calling this for a name that is NOT isolated records that a shared store
     * was handed out under it — see isolateIdentity().
     */
    static TokenManager& forIdentity(const QString& identity);

    /**
     * @brief Declare `identity` to have a PRIVATE, initially bootstrap-only store.
     *
     * Idempotent: isolating an already-isolated identity returns true and
     * changes nothing.
     *
     * Returns FALSE — and changes nothing — if forIdentity() already handed the
     * SHARED store out under this name. Callers must treat that as fatal for the
     * identity rather than continuing. The refusal is not defensive padding:
     * LogosAPIClient captures its store as a raw pointer at construction and
     * dereferences it on the hot path, so isolating after a client for the name
     * exists leaves one client on the ambient ring and another on the private
     * store — the precise "looks fixed, isn't" outcome this whole change exists
     * to avoid. Isolate before constructing anything for the identity.
     *
     * Returns false for an empty identity: "" is the not-an-identity value that
     * every un-named caller passes, and isolating it would isolate all of them
     * into one shared pseudo-store, which is worse than leaving them ambient.
     */
    static bool isolateIdentity(const QString& identity);

    /** @brief Whether `identity` has a private store. */
    static bool isIsolated(const QString& identity);

    /** @brief Every isolated identity. Diagnostics only. */
    static QStringList isolatedIdentities();

    /**
     * @brief The keys a private store is seeded with: the trust-root bootstrap.
     *
     * A private store starts empty of everything a caller could escalate with,
     * but NOT empty: a module's first call to an unknown target runs
     * `capability_module.requestModule`, and that call is itself authenticated
     * with the token stored under "capability_module" (and "core" for the
     * host-side channel). Withhold those and the very first exchange fails, so
     * an isolated identity could never obtain any token at all.
     *
     * These two keys, and only these two, are copied from instance() into a
     * private store when it is created. Every other module's root token — the
     * thing that made the ambient ring an escalation — is not.
     */
    static QStringList bootstrapKeys();

    /**
     * @brief Copy the bootstrap tokens from instance() into `identity`'s private
     *        store, for keys it does not already hold.
     *
     * Runs automatically when a private store is created, which is the ordering
     * a host already satisfies (bootstrap tokens are seeded before any module
     * loads). Exposed for the host that learns a bootstrap token later.
     *
     * @return the number of keys copied; 0 for a non-isolated identity.
     */
    static int seedBootstrapTokens(const QString& identity);

    /**
     * @brief Clear an isolated identity's private store and re-seed the
     *        bootstrap, e.g. when the plugin behind it is unloaded.
     *
     * The store OBJECT is immortal — a client mid-flight must never dereference
     * freed memory — so what has a lifetime is its CONTENTS. After a reload the
     * identity would otherwise present per-target tokens minted for its previous
     * incarnation; the provider rejects them and the existing rejection-driven
     * re-exchange heals it in one retry, so skipping this costs latency and log
     * noise rather than correctness. Do it anyway.
     *
     * Deliberately a no-op returning false for a NON-isolated identity: that
     * store is the shared ring, and clearing it would take every other
     * identity's tokens — including the host's — with it.
     */
    static bool resetIdentity(const QString& identity);

    /**
     * @brief Save a token with the given key
     * @param key The identifier for the token
     * @param token The token value to store
     */
    void saveToken(const QString& key, const QString& token);

    /**
     * @brief Save a token — const char* overload (resolves ambiguity, delegates to QString)
     */
    void saveToken(const char* key, const char* token)
        { saveToken(QString(key), QString(token)); }

    /**
     * @brief Save a token with the given key (std::string overload)
     */
    void saveToken(const std::string& key, const std::string& token);

    /**
     * @brief Retrieve a token by key
     * @param key The identifier for the token
     * @return QString The token value, or empty string if not found
     */
    QString getToken(const QString& key) const;

    /**
     * @brief Retrieve a token — const char* overload (resolves ambiguity, delegates to QString)
     */
    QString getToken(const char* key) const
        { return getToken(QString(key)); }

    /**
     * @brief Retrieve a token by key (std::string overload)
     * @return std::string The token value, or empty string if not found
     */
    std::string getToken(const std::string& key) const;

    /**
     * @brief Check if a token exists for the given key
     * @param key The identifier to check
     * @return bool True if token exists, false otherwise
     */
    bool hasToken(const QString& key) const;

    /**
     * @brief hasToken — const char* overload (resolves ambiguity, delegates to QString)
     */
    bool hasToken(const char* key) const
        { return hasToken(QString(key)); }

    /**
     * @brief Check if a token exists for the given key (std::string overload)
     */
    bool hasToken(const std::string& key) const;

    /**
     * @brief Remove a token by key
     * @param key The identifier for the token to remove
     * @return bool True if token was removed, false if it didn't exist
     */
    bool removeToken(const QString& key);

    /**
     * @brief removeToken — const char* overload (resolves ambiguity, delegates to QString)
     */
    bool removeToken(const char* key)
        { return removeToken(QString(key)); }

    /**
     * @brief Remove a token by key (std::string overload)
     */
    bool removeToken(const std::string& key);

    /**
     * @brief Clear all tokens
     */
    void clearAllTokens();

    /**
     * @brief Get all token keys
     * @return QList<QString> List of all token keys
     */
    QList<QString> getTokenKeys() const;

    /**
     * @brief Get all token keys (std::string flavour)
     *
     * Named rather than overloaded because C++ cannot overload on return type
     * alone. Exists for the Qt-free callers — lp_token_keys() and, through it,
     * any language SDK — so the conversion lives here next to the store rather
     * than being retyped at each boundary.
     */
    std::vector<std::string> getTokenKeysStd() const;

    /**
     * @brief Get the number of stored tokens
     * @return int Number of tokens stored
     */
    int tokenCount() const;

signals:
    /**
     * @brief Emitted when a token is saved
     * @param key The key of the saved token
     */
    void tokenSaved(const QString& key);

    /**
     * @brief Emitted when a token is removed
     * @param key The key of the removed token
     */
    void tokenRemoved(const QString& key);

    /**
     * @brief Emitted when all tokens are cleared
     */
    void allTokensCleared();

private:
    /**
     * @brief Private constructor for singleton pattern
     * @param parent Parent QObject
     *
     * Still private with per-identity stores in the picture, and that is what
     * forces forIdentity()/isolateIdentity() to be static MEMBERS rather than
     * free functions or a helper struct: only a member can construct one. A
     * public constructor would also defeat the purpose — nothing would make two
     * callers naming the same identity find the SAME store, and lp_client_create's
     * frozen signature has no way to be handed one.
     */
    explicit TokenManager(QObject *parent = nullptr);

    /**
     * @brief Private destructor
     *
     * Private, so a per-identity store cannot be deleted from outside — which is
     * exactly the intended lifetime. Private stores are heap-allocated and never
     * destroyed: LogosAPIClient holds its store by raw pointer and touches it
     * from async continuations that can fire after their caller returned, and
     * lp_client_destroy already defers client teardown to the owner thread and
     * documents that the client simply leaks if that loop never runs again. A
     * refcounted store would put destruction order on that path for a security
     * object. One QHash per isolated identity is not a cost worth that.
     */
    ~TokenManager();

    // Delete copy constructor and assignment operator to enforce singleton
    TokenManager(const TokenManager&) = delete;
    TokenManager& operator=(const TokenManager&) = delete;

    /**
     * @brief Hash map storing tokens by key.
     *
     * DIRECTION-MIXED, AND THEREFORE NEVER A CALLER ORACLE. One flat map with no
     * direction tag, written from both sides of every relationship:
     *
     *   * OUTBOUND — LogosAPIClient stores the token it will PRESENT to a callee
     *     under the CALLEE's name (logos_api_client.cpp:176, async twin :332).
     *   * INBOUND — a token RECEIVED from a caller is stored under the CALLER's
     *     name (lp_module_accept_token -> logos_protocol.cpp:635, and
     *     LogosProviderBase::informModuleToken in logos-plugin-qt).
     *
     * Same key namespace, last write wins. So a reverse lookup here — "which key
     * holds this token, therefore who is calling me" — can name a module we CALL
     * as the module CALLING us. That is affirmatively wrong, and worse than
     * declining to answer. ModuleProxy::m_tokens is the caller-keyed,
     * inbound-only record; anything that needs to NAME a caller uses that.
     *
     * The two directions do not currently collide in the DEFAULT topology, but
     * only by accident of linkage: a module cdylib links its own copy of this
     * library, so its outbound writes land in the cdylib image's instance()
     * while the host image's store takes the inbound ones. Any single-image
     * configuration — an in-process plugin host, the shared-runtime migration,
     * this test suite — puts both in one map again.
     */
    QHash<QString, QString> m_tokens;

    /**
     * @brief Mutex for thread-safe access to tokens
     */
    mutable QMutex m_mutex;
};

#endif // TOKEN_MANAGER_H 