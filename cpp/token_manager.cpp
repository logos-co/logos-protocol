#include "token_manager.h"
#include <QMutexLocker>
#include <QSet>

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
    QMutexLocker locker(&m_mutex);
    m_outbound[key] = token;
    // THE CREDENTIAL SHIM. See the note at the declaration: this is what keeps
    // every existing anchor writer working while the credential becomes a value
    // of its own. It writes the credential IN ADDITION TO the outbound entry,
    // never instead of it, because getToken("core") is still how
    // ModuleProxy::informModuleToken's callers and LogosAPIClient's
    // capability-handshake read it.
    if (bootstrapKeys().contains(key)) m_credential = token;
    emit tokenSaved(key);
}

void TokenManager::saveToken(const std::string& key, const std::string& token)
{
    saveToken(QString::fromStdString(key), QString::fromStdString(token));
}

QString TokenManager::getToken(const QString& key) const
{
    QMutexLocker locker(&m_mutex);
    // m_outbound only. A fall-through to m_inbound here would hand every module
    // its peers' credentials to present as its own — the same collision this
    // split removes, running the other way.
    return m_outbound.value(key, QString());
}

std::string TokenManager::getToken(const std::string& key) const
{
    return getToken(QString::fromStdString(key)).toStdString();
}

bool TokenManager::hasToken(const QString& key) const
{
    QMutexLocker locker(&m_mutex);
    return m_outbound.contains(key);
}

bool TokenManager::hasToken(const std::string& key) const
{
    return hasToken(QString::fromStdString(key));
}

bool TokenManager::removeToken(const QString& key)
{
    QMutexLocker locker(&m_mutex);
    if (m_outbound.contains(key)) {
        m_outbound.remove(key);
        // One value, two names: dropping one of them must not leave the
        // credential asserting a value the store no longer holds under any
        // bootstrap key. LogosAPIClient removes a rejected per-target token on
        // the re-exchange path (logos_api_client.cpp:139/:293), so this is
        // reachable from ordinary traffic rather than only from teardown.
        if (bootstrapKeys().contains(key)) resyncCredentialLocked();
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
    // ALL THREE. resetIdentity() documents that the credential goes too — a
    // reload re-mints and re-registers, so a surviving credential would be a
    // locked-out reload wearing the appearance of a working one — and the
    // inbound record must go for the same reason: it names the callers of the
    // PREVIOUS incarnation.
    m_outbound.clear();
    m_inbound.clear();
    m_credential.clear();
    emit allTokensCleared();
}

QList<QString> TokenManager::getTokenKeys() const
{
    QMutexLocker locker(&m_mutex);
    return m_outbound.keys();
}

std::vector<std::string> TokenManager::getTokenKeysStd() const
{
    QMutexLocker locker(&m_mutex);
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(m_outbound.size()));
    for (auto it = m_outbound.constBegin(); it != m_outbound.constEnd(); ++it)
        keys.push_back(it.key().toStdString());
    return keys;
}

int TokenManager::tokenCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_outbound.size();
}

/* ── the INBOUND half ───────────────────────────────────────────────────────
 *
 * Separate bodies rather than a direction parameter on the existing ones: a
 * parameter is a value a caller can get wrong (and default), while a separate
 * name is a symbol that either resolves or does not.
 */

bool TokenManager::saveInboundToken(const QString& caller, const QString& token)
{
    // Both refusals mirror ModuleProxy::saveToken. An empty token is the one
    // that matters: it would read as PRESENT to inbound().contains() while
    // authorizing nothing, which is the worst of both.
    if (caller.isEmpty() || token.isEmpty()) return false;
    QMutexLocker locker(&m_mutex);
    m_inbound[caller] = token;
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
    return m_credential;
}

QString TokenManager::inboundValue(const QString& caller) const
{
    QMutexLocker locker(&m_mutex);
    // m_inbound only, and this is THE line the split exists to keep honest: no
    // fall-through to m_outbound, however convenient it would look on the day
    // some caller comes up empty here.
    return m_inbound.value(caller, QString());
}

QStringList TokenManager::inboundKeyList() const
{
    QMutexLocker locker(&m_mutex);
    return QStringList(m_inbound.keyBegin(), m_inbound.keyEnd());
}

bool TokenManager::inboundContains(const QString& caller) const
{
    QMutexLocker locker(&m_mutex);
    return m_inbound.contains(caller);
}

int TokenManager::inboundCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_inbound.size();
}

void TokenManager::resyncCredentialLocked()
{
    for (const QString& key : bootstrapKeys()) {
        const QString value = m_outbound.value(key, QString());
        if (!value.isEmpty()) {
            m_credential = value;
            return;
        }
    }
    m_credential.clear();
} 