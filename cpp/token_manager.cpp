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
    // "core" is the host-side channel ModuleProxy::informModuleToken accepts as
    // trusted. Both are pre-seeded by every host before any module loads.
    return QStringList{QStringLiteral("core"), QStringLiteral("capability_module")};
}

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
        // Seed BEFORE publishing into the hash, so no caller can ever observe a
        // private store that has not been bootstrapped yet — and so the only
        // saveToken() issued under the registry lock is on an object nobody else
        // holds a handle to. That second part matters: saveToken emits
        // tokenSaved, and an emit under a lock is a re-entrancy hazard in
        // general. Here it cannot be one, because a brand-new unpublished store
        // has no connections for the emit to reach.
        TokenManager* store = new TokenManager(nullptr);
        for (const QString& key : bootstrapKeys()) {
            const QString token = instance().getToken(key);
            if (!token.isEmpty())
                store->saveToken(key, token);
        }
        it = registry().stores.insert(identity, store);
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

int TokenManager::seedBootstrapTokens(const QString& identity)
{
    // Check isolation BEFORE calling forIdentity: for a name that is not
    // isolated, forIdentity would record a shared vend and thereby make future
    // isolation of that name impossible. Seeding is a no-op there anyway, and a
    // no-op must not have that side effect.
    if (!isIsolated(identity)) return 0;

    // Resolving the store creates and seeds it on the first call; the loop then
    // tops up any bootstrap key it is still missing, which is the case a host
    // that learned a bootstrap token late needs.
    TokenManager& store = forIdentity(identity);
    if (&store == &instance()) return 0;   // belt and braces

    int copied = 0;
    for (const QString& key : bootstrapKeys()) {
        if (store.hasToken(key)) continue;
        const QString token = instance().getToken(key);
        if (token.isEmpty()) continue;
        store.saveToken(key, token);
        ++copied;
    }
    return copied;
}

bool TokenManager::resetIdentity(const QString& identity)
{
    if (!isIsolated(identity)) return false;
    TokenManager& store = forIdentity(identity);
    if (&store == &instance()) return false;   // belt and braces
    store.clearAllTokens();
    seedBootstrapTokens(identity);
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