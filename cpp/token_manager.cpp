#include "token_manager.h"
#include <QDebug>
#include <QMutexLocker>
#include <QSet>

/* -- THE ABI TRIPWIRE -------------------------------------------------------
 *
 * A TokenManager is allocated by one image and mutated by another: a module
 * plugin links its own copy of every accessor in this file and runs it on the
 * object the HOST image constructed (LogosAPI::getTokenManager()). The host is
 * one package, each module is its own .lgx, and they are built months apart.
 * So a member added here is an ABI break that shows up as a mutex CAS at the
 * wrong offset -- measured as a permanent deadlock in a shipped host process,
 * with no diagnostic anywhere (evaluateProtocolGate compares MAJOR only, and
 * logos_module_get_protocol_version() is called by nobody).
 *
 * This reference struct is the layout every module in the field was compiled
 * against. Direction lives in the KEY namespace precisely so that this line
 * never has to change; if it does have to change, that is a MAJOR bump and a
 * fleet-wide rebuild, not a MINOR.
 *
 * sizeof-equality is what a static_assert can see. The ORDER of the two members
 * is pinned by tests/protocol/test_token_manager_abi.cpp, which measures the
 * offsets. */
namespace {
struct TokenManagerAbiReference : QObject {
    QHash<QString, QString> tokens;
    mutable QMutex mutex;
};
}  // namespace
static_assert(sizeof(TokenManager) == sizeof(TokenManagerAbiReference),
              "TokenManager's layout is frozen: this object is allocated by the "
              "host image and mutated by module images built against other "
              "revisions of this header. Adding a member moves m_mutex and "
              "deadlocks them. Encode new state in the key namespace instead.");

TokenManager& TokenManager::instance()
{
    static TokenManager instance;
    return instance;
}

// ── the per-identity store registry ─────────────────────────────────────────
//
// Process-wide (per-image, like instance() itself) and file-local: nothing
// outside this translation unit can reach a store except through forIdentity(),
// which is what keeps "same identity ⇒ same store" true.
//
// `vendedShared` is why the registry needs three sets rather than two. It
// records every name forIdentity() answered with instance(), so isolateIdentity()
// can refuse a name whose clients are already pointing at the ambient ring
// instead of silently producing a split brain.
namespace {

struct StoreRegistry {
    QMutex mutex;
    QSet<QString> isolated;
    QSet<QString> vendedShared;
    QHash<QString, TokenManager*> stores;
};

StoreRegistry& registry()
{
    static StoreRegistry r;
    return r;
}

} // namespace

QStringList TokenManager::bootstrapKeys()
{
    // "capability_module" authenticates the requestModule handshake itself;
    // "core" is the channel ModuleProxy::informModuleToken accepts as trusted.
    // These are the keys an identity's OWN credential is installed under by
    // adoptCredential(); they are no longer copied from anywhere.
    return QStringList{QStringLiteral("core"), QStringLiteral("capability_module")};
}

namespace {

// Is `value` the HOST's anchor — the value instance() holds under a bootstrap
// key? Used to refuse an adoption that would re-create the copy this change
// removes, and to report identities that acquired it some other way.
//
// A plain == rather than a constant-time compare, deliberately: this is a
// host-side administrative check between two values the HOST already holds, not
// an authorization decision on an attacker-supplied token. The constant-time
// path is ModuleProxy::authorize and stays there.
bool isHostAnchorValue(const QString& value)
{
    if (value.isEmpty()) return false;
    // credential(), not getToken(bootstrapKey): the anchor is ONE value under
    // two role labels, and asking for it by that name is what stops this from
    // being a reverse lookup over a key namespace.
    return TokenManager::instance().credential() == value;
}

} // namespace

TokenManager& TokenManager::forIdentity(const QString& identity)
{
    QMutexLocker locker(&registry().mutex);

    if (identity.isEmpty() || !registry().isolated.contains(identity)) {
        // The default, and the whole reason this change is inert until a host
        // opts a name in: the SAME object instance() returns, not a copy of it.
        registry().vendedShared.insert(identity);
        return instance();
    }

    auto it = registry().stores.find(identity);
    if (it == registry().stores.end()) {
        // BORN EMPTY, and that is the whole of the fix on this side.
        //
        // This used to copy instance()'s tokens for bootstrapKeys() into the
        // new store. Those values are the HOST's, so every isolated identity
        // came into existence holding the host's anchor: it authorized as the
        // host at any callee (ModuleProxy::authorize -> callerHostAnchorJson)
        // and satisfied informModuleToken's trusted-channel gate, which is a
        // write into another module's token map. Isolating a name is the host
        // declaring that this caller must NOT have the host's authority; the
        // seed handed it exactly that.
        //
        // An empty store is a store that cannot call anything until the host
        // gives the identity its OWN credential — see adoptCredentialFor(), and
        // logos::admitConsumer in logos-plugin-qt, which mints it, registers it
        // with capability_module and adopts it in that order.
        //
        // No saveToken() under the registry lock any more, which also retires
        // the re-entrancy note that used to be here: saveToken emits
        // tokenSaved, and an emit under a lock is a hazard in general.
        it = registry().stores.insert(identity, new TokenManager(nullptr));
    }
    return *it.value();
}

bool TokenManager::isolateIdentity(const QString& identity)
{
    if (identity.isEmpty()) return false;

    QMutexLocker locker(&registry().mutex);
    if (registry().isolated.contains(identity)) return true;   // idempotent
    if (registry().vendedShared.contains(identity)) return false;  // too late
    registry().isolated.insert(identity);
    return true;
}

bool TokenManager::isIsolated(const QString& identity)
{
    QMutexLocker locker(&registry().mutex);
    return registry().isolated.contains(identity);
}

QStringList TokenManager::isolatedIdentities()
{
    QMutexLocker locker(&registry().mutex);
    QStringList out(registry().isolated.constBegin(), registry().isolated.constEnd());
    out.sort();
    return out;
}

void TokenManager::adoptCredential(const QString& credential)
{
    // Empty is a no-op: an empty value under a bootstrap key reads as PRESENT
    // to hasToken() and authorizes nothing, which is the worst of both.
    if (credential.isEmpty()) return;
    for (const QString& key : bootstrapKeys())
        saveToken(key, credential);
}

bool TokenManager::adoptCredentialFor(const QString& identity, const QString& credential)
{
    if (identity.isEmpty() || credential.isEmpty()) return false;

    // Check isolation BEFORE calling forIdentity: for a name that is not
    // isolated, forIdentity would record a shared vend and thereby make future
    // isolation of that name impossible. A refusal must not have that side
    // effect (same rule the old seedBootstrapTokens followed).
    if (!isIsolated(identity)) return false;

    // Refusing the host's own anchor is the sanctioned-API half of closing the
    // elevation. Without it, `adoptCredentialFor(x, hostAnchor)` would be a
    // one-line way to reintroduce exactly what forIdentity stopped doing.
    if (isHostAnchorValue(credential)) return false;

    TokenManager& store = forIdentity(identity);
    if (&store == &instance()) return false;   // belt and braces
    store.adoptCredential(credential);
    return true;
}

QStringList TokenManager::identitiesSharingHostAnchor()
{
    QStringList names;
    {
        QMutexLocker locker(&registry().mutex);
        names = QStringList(registry().isolated.constBegin(),
                            registry().isolated.constEnd());
    }

    // forIdentity() takes the registry lock itself, so the snapshot above is
    // released first rather than recursing on a non-recursive QMutex.
    QStringList out;
    for (const QString& name : names) {
        TokenManager& store = forIdentity(name);
        if (&store == &instance()) continue;
        if (isHostAnchorValue(store.credential()))
            out.append(name);
    }
    out.sort();
    return out;
}

bool TokenManager::resetIdentity(const QString& identity)
{
    if (!isIsolated(identity)) return false;
    TokenManager& store = forIdentity(identity);
    if (&store == &instance()) return false;   // belt and braces
    // No re-seed. The credential is cleared along with everything else, and the
    // caller must adopt the NEW one it just minted and registered — a reload
    // that re-registers invalidates the previous credential at the target, so
    // keeping it here would be a locked-out reload that looks like a live one.
    store.clearAllTokens();
    return true;
}

TokenManager::TokenManager(QObject *parent)
    : QObject(parent)
{
}

TokenManager::~TokenManager()
{
}

void TokenManager::saveToken(const QString& key, const QString& token)
{
    // The namespace guard, on the OUTBOUND door. `key` arrives from the wire on
    // the paths that matter (capability_module names the peer in
    // informModuleToken, and lp_token_save takes whatever the C ABI was handed),
    // so a name carrying the direction-namespace character would be a way to
    // write an outbound value into an inbound key and re-create the collision
    // this split removes. Loud, because a legitimate module name can never
    // contain a C0 control character and there is therefore no benign case.
    if (isReservedKey(key)) {
        qWarning() << "TokenManager: refusing to save a token under a reserved key"
                   << "(the direction namespace is not addressable from outside)";
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_tokens[key] = token;
    // No separate credential to write: credential() reads whichever
    // bootstrapKeys() key is set, which is exactly where this call just put it.
    // That is THE CREDENTIAL SHIM -- see the note at the declaration -- and
    // deriving rather than caching is what makes it survive a store written by
    // an image built against a different revision of this header.
    emit tokenSaved(key);
}

void TokenManager::saveToken(const std::string& key, const std::string& token)
{
    saveToken(QString::fromStdString(key), QString::fromStdString(token));
}

QString TokenManager::getToken(const QString& key) const
{
    // The outbound namespace only. Refusing a reserved key here is the read-side
    // half of the same guard: without it, getToken(inboundKey(x)) would hand a
    // caller a token that caller was ISSUED, to present as if it were its own --
    // the collision this split removes, running the other way.
    if (isReservedKey(key)) return QString();
    QMutexLocker locker(&m_mutex);
    return m_tokens.value(key, QString());
}

std::string TokenManager::getToken(const std::string& key) const
{
    return getToken(QString::fromStdString(key)).toStdString();
}

bool TokenManager::hasToken(const QString& key) const
{
    if (isReservedKey(key)) return false;
    QMutexLocker locker(&m_mutex);
    return m_tokens.contains(key);
}

bool TokenManager::hasToken(const std::string& key) const
{
    return hasToken(QString::fromStdString(key));
}

bool TokenManager::removeToken(const QString& key)
{
    // Outbound door: an inbound entry is not removable through it, for the same
    // reason it is not readable through it.
    if (isReservedKey(key)) return false;
    QMutexLocker locker(&m_mutex);
    if (m_tokens.contains(key)) {
        m_tokens.remove(key);
        // No credential to resync. credential() derives from whichever
        // bootstrapKeys() key is still set, so dropping one of the two names can
        // no longer leave a cached value asserting a token the store does not
        // hold. LogosAPIClient removes a rejected per-target token on the
        // re-exchange path (logos_api_client.cpp:139/:293), so that drift was
        // reachable from ordinary traffic rather than only from teardown.
        emit tokenRemoved(key);
        return true;
    }
    return false;
}

bool TokenManager::removeToken(const std::string& key)
{
    return removeToken(QString::fromStdString(key));
}

void TokenManager::clearAllTokens()
{
    QMutexLocker locker(&m_mutex);
    // BOTH NAMESPACES, and the credential with them. resetIdentity() documents
    // why: a reload re-mints and re-registers, so a surviving credential would
    // be a locked-out reload wearing the appearance of a working one, and the
    // inbound record names the callers of the PREVIOUS incarnation. One clear()
    // takes all three because all three live in this map.
    m_tokens.clear();
    emit allTokensCleared();
}

QList<QString> TokenManager::getTokenKeys() const
{
    QMutexLocker locker(&m_mutex);
    // OUTBOUND ONLY. This is the roster lp_token_keys() publishes to a granted
    // token registry, and leaking the inbound namespace into it would publish
    // the names of everyone who may call US as if they were modules we can call.
    QList<QString> keys;
    keys.reserve(m_tokens.size());
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        if (!isReservedKey(it.key())) keys.append(it.key());
    return keys;
}

std::vector<std::string> TokenManager::getTokenKeysStd() const
{
    QMutexLocker locker(&m_mutex);
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(m_tokens.size()));
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        if (!isReservedKey(it.key())) keys.push_back(it.key().toStdString());
    return keys;
}

int TokenManager::tokenCount() const
{
    QMutexLocker locker(&m_mutex);
    int n = 0;
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        if (!isReservedKey(it.key())) ++n;
    return n;
}

/* ── the INBOUND half ───────────────────────────────────────────────────────
 *
 * Separate bodies rather than a direction parameter on the existing ones: a
 * parameter is a value a caller can get wrong (and default), while a separate
 * name is a symbol that either resolves or does not.
 */

bool TokenManager::saveInboundToken(const QString& caller, const QString& token)
{
    // The first two refusals mirror ModuleProxy::saveToken. An empty token is
    // the one that matters: it would read as PRESENT to inbound().contains()
    // while authorizing nothing, which is the worst of both.
    if (caller.isEmpty() || token.isEmpty()) return false;
    // The third is the namespace guard on the INBOUND door. `caller` is named by
    // capability_module over RPC; a name carrying the namespace character would
    // let it address a key other than its own inbound slot.
    if (isReservedKey(caller)) {
        qWarning() << "TokenManager: refusing an inbound token for a caller name "
                      "carrying the reserved direction namespace";
        return false;
    }
    QMutexLocker locker(&m_mutex);
    m_tokens[inboundKey(caller)] = token;
    return true;
}

bool TokenManager::saveInboundToken(const std::string& caller, const std::string& token)
{
    return saveInboundToken(QString::fromStdString(caller),
                            QString::fromStdString(token));
}

QString TokenManager::credential() const
{
    QMutexLocker locker(&m_mutex);
    return credentialLocked();
}

QString TokenManager::inboundValue(const QString& caller) const
{
    if (isReservedKey(caller)) return QString();
    QMutexLocker locker(&m_mutex);
    // The inbound namespace only, and this is THE line the split exists to keep
    // honest: no fall-through to the bare key, however convenient it would look
    // on the day some caller comes up empty here.
    return m_tokens.value(inboundKey(caller), QString());
}

QStringList TokenManager::inboundKeyList() const
{
    QMutexLocker locker(&m_mutex);
    // The stored keys carry the namespace prefix; callers -- ModuleProxy's scan
    // among them -- want the bare caller names, so strip it here rather than
    // letting the encoding escape the class.
    //
    // THIS READS "RESERVED" AS "INBOUND", AND THAT IS ONLY TRUE WHILE THERE IS
    // EXACTLY ONE RESERVED NAMESPACE. isReservedKey() asks whether a key
    // carries the namespace CHARACTER anywhere; inboundKey() is the only
    // producer of such a key today, so the two coincide and a fixed-width strip
    // is exact. Introduce a second namespace -- U+0001 "out", U+0001 "meta",
    // anything -- and this silently mis-reports every key in it AS AN INBOUND
    // CALLER, with a mangled name, straight into ModuleProxy's authorization
    // scan. inboundCount() below makes the same assumption and would over-count
    // the same way.
    //
    // WHAT TO DO INSTEAD, at that point and not before: match the specific
    // prefix (`it.key().startsWith(inboundKey(QString()))`) rather than the
    // character class, in both functions. It is not written that way now
    // because the guard would be dead code today and an untested branch in a
    // security scan is worse than an invariant stated where it can be read.
    const int skip = inboundKey(QString()).size();
    QStringList out;
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        if (isReservedKey(it.key())) out.append(it.key().mid(skip));
    return out;
}

bool TokenManager::inboundContains(const QString& caller) const
{
    if (isReservedKey(caller)) return false;
    QMutexLocker locker(&m_mutex);
    return m_tokens.contains(inboundKey(caller));
}

int TokenManager::inboundCount() const
{
    QMutexLocker locker(&m_mutex);
    // "reserved" == "inbound" only while one namespace exists -- see the note
    // in inboundKeyList() above, which this shares.
    int n = 0;
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        if (isReservedKey(it.key())) ++n;
    return n;
}

QString TokenManager::credentialLocked() const
{
    // ONE VALUE UNDER TWO ROLE LABELS, read rather than cached. Every anchor
    // writer in the fleet installs it with saveToken() under both bootstrap
    // keys, so this is not an inference about where it might be -- it is the
    // place it is put. Deriving it is also what makes the answer correct on a
    // store written by an image built against another revision of this header.
    for (const QString& key : bootstrapKeys()) {
        const QString value = m_tokens.value(key, QString());
        if (!value.isEmpty()) return value;
    }
    return QString();
} 