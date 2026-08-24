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
     * @brief The keys an identity's CREDENTIAL is installed under: the
     *        trust-root bootstrap.
     *
     * A private store is born EMPTY, and it must not STAY empty: a caller's
     * first call to an unknown target runs `capability_module.requestModule`,
     * and that call is itself authenticated with the token stored under
     * "capability_module". "core" is the other half — the channel
     * ModuleProxy::informModuleToken accepts as trusted, which an isolated
     * PROVIDER identity needs in order to be told about its own callers.
     * Withhold both and the very first exchange fails at
     * ModuleProxy::authorize's empty-token check, so an isolated identity could
     * never obtain any token at all: isolation would be a LOCKOUT.
     *
     * WHAT USED TO HAPPEN HERE, AND WHY IT WAS WRONG. These two keys were
     * COPIED from instance() when a private store was created. That value is
     * the HOST's. A consumer presenting it hits an anchor key at the callee's
     * proxy, so ModuleProxy::authorize answers logos::callerHostAnchorJson() —
     * a sandboxed in-process view wearing the host's authority. And the half
     * that was never latent: informModuleToken's trusted-channel gate compares
     * against these same two keys, so any holder of the copy could push
     * arbitrary (name, token) pairs into another module's token map with three
     * public calls and no generated glue. The credential an identity presents
     * must be ITS OWN.
     *
     * So these are the keys adoptCredential() writes, and the only keys any
     * store is bootstrapped with. This function is their single owner: no host,
     * binding or language backend should spell the pair a second time.
     */
    static QStringList bootstrapKeys();

    /**
     * @brief Install `credential` as THIS store's identity credential — its
     *        value under every bootstrapKeys() key.
     *
     * THE RULE, and it is not new: an identity's store carries THAT IDENTITY'S
     * OWN host-issued credential under the bootstrap keys. Every other image in
     * the system already works this way. A module image writes its own
     * `authToken` under both keys (LogosAPIProvider::seedHandshakeTrustAnchor);
     * a ui-host process writes the per-spawn credential its parent minted and
     * registered for it. The in-process private store was the ONE store in the
     * system seeded with somebody else's credential.
     *
     * Both directions fall out of the one write:
     *   * OUTBOUND — the identity presents the credential to
     *     capability_module, whose proxy finds it in its caller-keyed inbound
     *     record (written by informModuleToken) rather than on an anchor key,
     *     so the caller resolves to {"kind":"module","name":<identity>} instead
     *     of {"kind":"host"}.
     *   * INBOUND — capability_module pushes to a provider identity using
     *     `tokenManager->getToken(moduleName)`, which IS this credential, so
     *     informModuleToken's trusted-channel gate still passes.
     *
     * An empty credential is a no-op: writing one would leave a value that
     * reads as present to hasToken() while authorizing nothing.
     */
    void adoptCredential(const QString& credential);

    /**
     * @brief adoptCredential() for an isolated identity's private store.
     *
     * REFUSES — returns false and writes nothing — in two cases, and both
     * refusals are the mechanism rather than defensive padding:
     *
     *   * `identity` is NOT isolated. That store is the ambient ring, and
     *     installing a credential there would hand it to every un-isolated
     *     caller in the image, the host included.
     *   * `credential` equals instance()'s value under any bootstrap key, i.e.
     *     it IS the host's anchor. Adopting the host's anchor as your own is
     *     exactly the bug this replaces, and it must not be reachable through
     *     the sanctioned API.
     *
     * Deliberately does not vend the store for a non-isolated name: a refusal
     * must not make later isolation impossible (see isolateIdentity()).
     */
    static bool adoptCredentialFor(const QString& identity, const QString& credential);

    /**
     * @brief Isolated identities whose store holds a bootstrap value equal to
     *        instance()'s.
     *
     * Diagnostics, and a one-line assertion for a host's CI: after every
     * consumer has been admitted this must be EMPTY. adoptCredentialFor()
     * closes the sanctioned route to the host anchor; a host that copies the
     * anchor by hand still can, and this is the instrument that sees it.
     */
    static QStringList identitiesSharingHostAnchor();

    /**
     * @brief Clear an isolated identity's private store, e.g. when the plugin
     *        behind it is unloaded.
     *
     * The store OBJECT is immortal — a client mid-flight must never dereference
     * freed memory — so what has a lifetime is its CONTENTS. After a reload the
     * identity would otherwise present per-target tokens minted for its previous
     * incarnation.
     *
     * CLEARS THE CREDENTIAL TOO, and that is a deliberate change from the
     * version of this that re-seeded the bootstrap from instance(). A reload
     * re-mints and re-registers, which invalidates the previous credential at
     * the target (ModuleProxy::saveToken overwrites m_tokens[name]); leaving a
     * stale credential in the store would therefore be a locked-out reload that
     * looks like a working one. The caller must adopt the NEW credential after
     * this — see logos::reissueConsumerCredential in logos-plugin-qt, which is
     * the one place that sequences mint, register, reset and adopt.
     *
     * Deliberately a no-op returning false for a NON-isolated identity: that
     * store is the shared ring, and clearing it would take every other
     * identity's tokens — including the host's — with it.
     */
    static bool resetIdentity(const QString& identity);

    /* ── DIRECTION: three roles, and they are three different things ─────────
     *
     *   OUTBOUND    callee -> the token I present when I call that callee.
     *               Written by LogosAPIClient on the first exchange
     *               (logos_api_client.cpp:201, async twin :357) and read back
     *               at :124 / :313. Keyed by the module I am CALLING.
     *   INBOUND     caller -> the token I issued to that caller, which it
     *               presents when it calls ME. Written by the provider's
     *               informModuleToken. Keyed by the module CALLING me.
     *   CREDENTIAL  MY OWN host-issued credential. One value, not a map — see
     *               credential() below for why that is a finding rather than a
     *               choice.
     *
     * WHAT SHARING ONE MAP COST. Until this split all three lived in one flat
     * QHash<QString,QString> with no direction tag, and ModuleProxy::authorize
     * accepted anything in it. So a token cached in order to CALL x authorized x
     * to call ME: capability_module mints ONE value for <m -> b>, m caches it
     * outbound under "b" and b records it inbound under "m", after which b may
     * call m and m accepts — no handshake, nothing logged, and
     * capability_module's access policy never consulted. The grant graph the
     * policy is written against is directed; the enforcement was not. Reachable
     * in the DEFAULT topology, not only under single-image: a module loaded by
     * logos-module-loader-qt runs in its own process and one TokenManager there
     * takes all three writes. tests/protocol/test_token_direction.cpp is the
     * detector, with the numbers.
     *
     * WHAT KEEPS IT SPLIT, and it is not anyone remembering to:
     *
     *   * ModuleProxy::authorize is handed an InboundView and a credential
     *     STRING — never a TokenManager* — so no expression inside the scan can
     *     name THIS PROXY'S outbound map. That is a property of the injected
     *     store, NOT of the class: TokenManager::instance() is a public static,
     *     so any code that wants the process-wide outbound map can still reach
     *     it in one expression. What the scan cannot do is reach it by
     *     ACCIDENT, which is the failure this split is about — the pre-split
     *     hole was one map serving both directions, not a deliberate lookup.
     *   * getToken() never falls through to the inbound map, so a token some
     *     caller was issued can never be presented as if it were ours.
     *   * The two halves cannot produce the same STORAGE KEY even for the same
     *     peer name: outbound is filed under the bare name, inbound under
     *     inboundKey(caller), and every door refuses a key that carries the
     *     namespace character. See the layout note in the private section --
     *     the halves are two key namespaces in ONE QHash, not two members,
     *     because this object crosses the host/module package boundary and
     *     adding a member to it deadlocked real hosts.
     *
     * WHY THE UNADORNED SPELLINGS ARE THE OUTBOUND ONES. saveToken / getToken /
     * hasToken / removeToken / getTokenKeys / tokenCount keep meaning OUTBOUND,
     * deliberately: a future writer who reaches for the familiar name intending
     * an inbound grant grants NOTHING, and the failure is a single visibly
     * rejected call. The opposite default would over-grant, silently.
     *
     * THE ONE CHANGE THAT MUST NEVER BE MADE: a read-side fall-through between
     * the two halves. One line puts the collision back and it would look like a
     * convenience.
     */

    /**
     * @brief Save an OUTBOUND token: what I present when I call `key`.
     *
     * `key` is the module I am CALLING. This does NOT authorize `key` to call
     * me — see saveInboundToken() for that, and the DIRECTION note above for
     * why reaching for this one by mistake fails closed.
     *
     * THE CREDENTIAL SHIM: when `key` is one of bootstrapKeys() this ALSO
     * installs `token` as this store's credential(), which is what lets every
     * existing anchor writer keep working untouched — logos-module-loader-qt's
     * module_initializer.cpp:169-170, logos-plugin-qt's
     * seedHandshakeTrustAnchor and its generated glue's
     * logos_module_accept_token("core")/("capability_module"), logos-qt-sdk's
     * LpBridge::syncFromApi. Retire the shim by moving those four to
     * adoptCredential(); until then this is the only reason they are not a
     * same-wave breaking change.
     *
     * The shim cannot be abused into re-creating the collision by caching a
     * per-target token for a module literally NAMED "core" or
     * "capability_module": LogosAPIClient only ever writes here after a MISS on
     * getToken(objectName) (logos_api_client.cpp:124), and a store that has a
     * credential answers both those names non-empty, so :201 is never reached
     * for them. A store with no credential has nothing to clobber.
     *
     * REFUSES a `key` carrying the direction-namespace character, silently to
     * the caller and loudly in the log. `key` reaches here from the wire --
     * capability_module names the peer in informModuleToken -- so without the
     * refusal a caller could spell a name that files an OUTBOUND value into the
     * INBOUND namespace, which is the pre-split collision reconstructed by hand.
     * A module name cannot contain a C0 control character, so nothing legitimate
     * is refused.
     *
     * @param key The CALLEE this token is for
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
     * @brief Retrieve an OUTBOUND token: what I present when I call `key`.
     *
     * Reads the outbound half and the outbound half ONLY. It does not fall
     * through to the inbound map, and must never be made to: a token a caller
     * was issued is that caller's to present, not ours, and answering it here
     * would let any module present a peer's credential as its own.
     *
     * @param key The CALLEE whose token to look up
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
     * @brief Every OUTBOUND key: the modules this store can call.
     *
     * The roster lp_token_keys() publishes (gated on the "token_registry" host
     * service). Inbound callers are NOT here — see inbound().keys().
     *
     * @return QList<QString> List of all outbound token keys
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
     * @brief Get the number of OUTBOUND tokens stored.
     * @return int Number of outbound tokens stored
     */
    int tokenCount() const;

    /* ── the INBOUND half ────────────────────────────────────────────────────
     *
     * caller -> the token THIS store issued to that caller. The only store here
     * that can honestly answer "who is calling me", and the only one
     * ModuleProxy::authorize is allowed to see.
     */

    /**
     * @brief A read-only handle onto the INBOUND half, and only the inbound
     *        half.
     *
     * THIS IS THE MECHANISM, not a convenience wrapper. The authorization scan
     * receives one of these instead of the TokenManager it came from, so the
     * outbound accessors are not merely the wrong choice there — they cannot be
     * named at all. A rename would have relied on every future caller choosing
     * correctly; this does not.
     *
     * Copyable and non-owning: it is a pointer to a store whose address is
     * stable for the lifetime of the image (see forIdentity()). Only
     * TokenManager::inbound() can make one.
     */
    class InboundView
    {
    public:
        /** @brief The token issued to `caller`, or empty. NEVER falls through
         *         to the outbound map — that fall-through IS the bug this
         *         split removes. */
        inline QString token(const QString& caller) const;
        /** @brief Every caller this store has issued a token to. */
        inline QStringList keys() const;
        /** @brief Whether `caller` has been issued a token. */
        inline bool contains(const QString& caller) const;
        /** @brief How many callers have been issued a token. */
        inline int count() const;

    private:
        friend class TokenManager;
        explicit InboundView(const TokenManager* owner) : m_owner(owner) {}
        const TokenManager* m_owner;
    };

    /** @brief The inbound-only handle. See InboundView. */
    InboundView inbound() const { return InboundView(this); }

    /**
     * @brief Whether `key` carries the direction namespace, and is therefore
     *        refused by every door on this class.
     *
     * PUBLIC SO A REFUSAL CAN BE REPORTED, not so the encoding can be used.
     * saveToken() returns void — its signature is pinned by the same
     * cross-package ABI freeze the layout note below describes, so it cannot
     * grow a bool — and a C door that forwards to it therefore has nothing to
     * turn into a return code. lp_token_save answered LP_OK for a refused write
     * while lp_token_save_inbound answered LP_ERR_INVALID_ARG for the identical
     * one, and a module reading rc=0 went on believing it held a credential it
     * does not hold. This is what lets the outbound doors ask FIRST and answer
     * the same way.
     *
     * It is a predicate over the KEY SPACE, not over any token: it says which
     * names are addressable from outside, which is public information (no
     * module name may contain a C0 control character). It vends no encoding —
     * namespaceChar() and inboundKey() stay private — so nothing outside can
     * construct a reserved key, only recognise one.
     */
    static bool isReservedKey(const QString& key) { return key.contains(namespaceChar()); }

    /**
     * @brief Record that `caller` may present `token` when calling ME.
     *
     * The INBOUND door. Two production writers, both downstream of
     * ModuleProxy::informModuleToken, which admits nothing that did not
     * authenticate on the trusted core/capability channel first:
     *
     *   * the provider's informModuleToken in the HOST image (logos-plugin-qt
     *     LogosProviderBase::informModuleToken and
     *     QtProviderObject::informModuleToken), and
     *   * lp_token_save_inbound in the MODULE image, reached from the generated
     *     glue's informModuleToken through the module-impl export
     *     logos_module_accept_inbound_token (protocol 0.8). A Qt plugin links
     *     its own copy of this library, so the host's store and the plugin's
     *     are different objects and both have to be told.
     *
     * lp_token_save_inbound ALSO writes the outbound half when its image holds
     * the "token_registry" grant -- see the note on that function in
     * logos_protocol.h. That is not a leak between the halves: it is the one
     * receiver for whom the same wire message genuinely means the other
     * direction.
     *
     * Refuses an empty caller or an empty token, for the same reason
     * ModuleProxy::saveToken does: an empty value reads as PRESENT to
     * inbound().contains() while authorizing nothing. Refuses a `caller`
     * carrying the direction-namespace character for the reason saveToken()
     * gives: the name arrives over RPC and must not be able to address any key
     * but its own.
     *
     * @return true if it was recorded
     */
    bool saveInboundToken(const QString& caller, const QString& token);

    /**
     * @brief saveInboundToken — const char* overload.
     *
     * Not sugar: without it a literal pair is AMBIGUOUS between the QString and
     * std::string overloads and every `saveInboundToken("a", "b")` fails to
     * compile, exactly as saveToken's own const char* overload exists to
     * prevent. Found by building logos-qt-sdk's tests against this header, not
     * by reading it.
     */
    bool saveInboundToken(const char* caller, const char* token)
        { return saveInboundToken(QString(caller), QString(token)); }

    /** @brief saveInboundToken — std::string overload. */
    bool saveInboundToken(const std::string& caller, const std::string& token);

    /**
     * @brief THIS store's own host-issued credential — the trust anchor.
     *
     * A VALUE, not a map, and that is a finding rather than a design choice.
     * The anchor is genuinely both directions — presented outbound to
     * capability_module (logos_api_client.cpp:164/:341) and compared against
     * inbound (module_proxy.cpp's informModuleToken gate and authorize's
     * anchorHits) — which is exactly why it resists a two-way split and exactly
     * why it must not be a third MAP: a key living in two maps is a rename.
     *
     * It already was one value under two names. adoptCredential() writes ONE
     * credential under EVERY bootstrapKeys() key, those keys are role labels
     * rather than module names (token_manager.cpp:38-45), and
     * logos_caller_scope.h forbids the host arm from carrying a name for
     * precisely that reason. Making it a scalar is what it already is.
     *
     * And it is what dissolves the objection: what authorize() is HANDED is a
     * bare QString with no key attached, so nothing inside the scan can turn a
     * credential match into a module NAME -- which is the property that matters,
     * and the reason anchorHits is counted separately from moduleHits.
     *
     * It is DERIVED, not stored: the value under whichever bootstrapKeys() key
     * is set. That is not a weakening -- the credential has always also been in
     * the outbound half, because every writer in the fleet installs it with
     * saveToken("core", ...) and the shim below keeps that working -- and it is
     * what lets this answer correctly on a store written by an image built
     * against a different revision of this header. A cached member could not:
     * it read empty on a store an older image wrote, which refuses every
     * informModuleToken push.
     *
     * Written by adoptCredential(), and by saveToken() under a bootstrap key
     * (the shim documented there).
     */
    QString credential() const;

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

    /* -- THE LAYOUT IS FROZEN, AND IT IS FROZEN BY MEASUREMENT --------------
     *
     * This object is allocated by one image and MUTATED BY ANOTHER. That is not
     * a hypothetical: a module plugin statically links its own copy of this
     * library and therefore its own copy of every method below, then operates
     * on the TokenManager the HOST image constructed, reached through
     * LogosAPI::getTokenManager(). LogosProviderBase::informModuleToken and
     * logos-qt-sdk's LpBridge::syncFromApi are both plugin CODE running on a
     * host OBJECT. The host ships as one package and each module ships as its
     * own .lgx, installed independently, so the two are routinely built months
     * apart.
     *
     * An earlier revision of this file argued the opposite -- "no consumer ever
     * allocates a TokenManager and none needs sizeof(), so a consumer compiled
     * against the old header keeps working against the new library" -- and split
     * the store into three members on the strength of it. The premise is false:
     * the accessor bodies are the CONSUMER's, compiled against the CONSUMER's
     * header, so the member offsets they use are the consumer's too. Adding
     * m_inbound and m_credential moved m_mutex from +24 to +56, and the
     * version-mix matrix measured what that costs, on shipped artifacts:
     *
     *   * OLD host + NEW module. saveInboundToken compare-exchanges at this+56
     *     on a 32-byte object -- past the end, into adjacent BSS. In the
     *     measured host that word is boost::asio's openssl_init guard, whose
     *     value is 1, which is exactly Qt's dummyLocked() sentinel, so the fast
     *     path can never win and QBasicMutex::lockInternal() futex-waits
     *     forever. The module's host process deadlocks on the FIRST inbound
     *     token push and never serves another call -- while the daemon still
     *     reports it "loaded", "crashed": 0.
     *   * NEW host + OLD module. The old code CASes this+24, which post-split
     *     is m_inbound's QHash d-pointer. It survives only while that hash is
     *     empty (a null d reads as unlocked); the same deadlock appears the
     *     moment anything writes an inbound token first.
     *   * The ui seam, both ways. LpBridge::syncFromApi -> getToken hangs
     *     before the ui plugin ever reaches READY.
     *
     * None of it is detectable in band: evaluateProtocolGate compares MAJOR
     * only, logos_module_get_protocol_version() is exported by every module and
     * called by nobody, and both layouts answer "0.7.0".
     *
     * SO DIRECTION IS ENCODED IN THE KEY, NOT IN NEW FIELDS. One QHash and one
     * QMutex, at the offsets every shipped module was compiled against.
     * m_tokens holds both halves in disjoint key namespaces: an OUTBOUND entry
     * is filed under the bare peer name exactly as it always was, an INBOUND
     * entry under inboundKey(caller), which begins with a character no module
     * name can contain. The public surface is unchanged -- saveToken/getToken
     * are the outbound half, saveInboundToken/InboundView the inbound half,
     * neither reachable from the other -- so what the split bought is bought
     * here too; only the storage stopped being an ABI event.
     *
     * WHY A KEY NAMESPACE IS NOT THE OLD COLLISION WEARING A HAT. The pre-split
     * bug was that ONE key served both directions and ModuleProxy::authorize
     * accepted anything in the map. Here the two directions cannot produce the
     * same key even when the peer name is identical; the scan is handed an
     * InboundView that can only enumerate inbound keys; and every door refuses a
     * key carrying the namespace character (isReservedKey), so no wire-supplied
     * name can forge its way into the other half. tests/protocol/
     * test_token_direction.cpp and test_token_manager_abi.cpp are the detectors.
     *
     * ANYTHING ADDED HERE IS AN ABI BREAK ACROSS THE PACKAGE BOUNDARY. The
     * static_assert in token_manager.cpp is the tripwire: its reference struct
     * spells out the layout master shipped, so a new member cannot be added
     * quietly. */

    /**
     * @brief Every token this store holds, in two disjoint key namespaces.
     *
     * OUTBOUND -- key is the bare peer name: the token I present when I CALL it.
     * INBOUND  -- key is inboundKey(caller): the token I ISSUED to that caller.
     *
     * Never read directly by ModuleProxy. That is enforced by authorize() being
     * handed an InboundView and a credential string rather than this object,
     * and the view enumerates the inbound namespace only.
     */
    QHash<QString, QString> m_tokens;

    /**
     * @brief Mutex for thread-safe access to tokens
     *
     * MUST REMAIN THE LAST MEMBER, AND THE ONLY ONE AFTER m_tokens. See the
     * layout note above: a plugin built against a different revision of this
     * header locks at whatever offset ITS header put this at.
     */
    mutable QMutex m_mutex;

    /* -- the namespace that carries direction -------------------------------
     *
     * U+0001 (START OF HEADING). A module name is a metadata.json identifier
     * and a name on the wire; it cannot contain a C0 control character. Every
     * public door checks isReservedKey(), so a caller-supplied name -- which on
     * the inbound path arrives over RPC from capability_module -- can never be
     * spelled to land in the other half.
     *
     * Static inline, so no storage is added and nothing here is an ABI event.
     * They are also the ONLY inline bodies in this header that touch the
     * encoding, and they touch no member, which keeps the rule stated at the
     * top of the private section true: every member access goes through an
     * out-of-line symbol. */
    static QChar namespaceChar() { return QChar(u'\u0001'); }
    static QString inboundKey(const QString& caller)
    {
        return QString(namespaceChar()) + QStringLiteral("in")
             + QString(namespaceChar()) + caller;
    }

    /* The inbound reads, private so InboundView is the only way to reach them.
     * A nested class is a member and has access to the enclosing class's
     * private members, so no friend declaration is needed and no second public
     * spelling of the inbound half exists to be picked by accident. */
    QString inboundValue(const QString& caller) const;
    QStringList inboundKeyList() const;
    bool inboundContains(const QString& caller) const;
    int inboundCount() const;

    /* THIS store's credential, read with m_mutex already held.
     *
     * The credential is DERIVED, not a field, and that is what makes it answer
     * correctly on a store some other image wrote. It is the value under
     * whichever bootstrapKeys() key is set -- precisely where every anchor
     * writer in the fleet has always put it (module_initializer.cpp:169-170,
     * seedHandshakeTrustAnchor, the generated glue's
     * logos_module_accept_token("core"), LpBridge::syncFromApi, ui-host
     * main.cpp:212-213). A cached field got this wrong in BOTH mixed-package
     * directions: written by an old image it stayed empty and every
     * informModuleToken push was refused; written by a new image it landed
     * outside a 32-byte object. */
    QString credentialLocked() const;
};

inline QString TokenManager::InboundView::token(const QString& caller) const
{
    return m_owner->inboundValue(caller);
}

inline QStringList TokenManager::InboundView::keys() const
{
    return m_owner->inboundKeyList();
}

inline bool TokenManager::InboundView::contains(const QString& caller) const
{
    return m_owner->inboundContains(caller);
}

inline int TokenManager::InboundView::count() const
{
    return m_owner->inboundCount();
}

#endif // TOKEN_MANAGER_H 