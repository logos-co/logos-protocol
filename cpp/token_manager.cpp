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
    for (const QString& key : TokenManager::bootstrapKeys()) {
        if (TokenManager::instance().getToken(key) == value) return true;
    }
    return false;
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
        for (const QString& key : bootstrapKeys()) {
            if (isHostAnchorValue(store.getToken(key))) {
                out.append(name);
                break;
            }
        }
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
    m_tokens[key] = token;
    emit tokenSaved(key);
}

void TokenManager::saveToken(const std::string& key, const std::string& token)
{
    saveToken(QString::fromStdString(key), QString::fromStdString(token));
}

QString TokenManager::getToken(const QString& key) const
{
    QMutexLocker locker(&m_mutex);
    return m_tokens.value(key, QString());
}

std::string TokenManager::getToken(const std::string& key) const
{
    return getToken(QString::fromStdString(key)).toStdString();
}

bool TokenManager::hasToken(const QString& key) const
{
    QMutexLocker locker(&m_mutex);
    return m_tokens.contains(key);
}

bool TokenManager::hasToken(const std::string& key) const
{
    return hasToken(QString::fromStdString(key));
}

bool TokenManager::removeToken(const QString& key)
{
    QMutexLocker locker(&m_mutex);
    if (m_tokens.contains(key)) {
        m_tokens.remove(key);
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
    m_tokens.clear();
    emit allTokensCleared();
}

QList<QString> TokenManager::getTokenKeys() const
{
    QMutexLocker locker(&m_mutex);
    return m_tokens.keys();
}

std::vector<std::string> TokenManager::getTokenKeysStd() const
{
    QMutexLocker locker(&m_mutex);
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(m_tokens.size()));
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        keys.push_back(it.key().toStdString());
    return keys;
}

int TokenManager::tokenCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_tokens.size();
} 