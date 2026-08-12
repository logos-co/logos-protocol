#include "token_manager.h"
#include <QMutexLocker>

TokenManager& TokenManager::instance()
{
    static TokenManager instance;
    return instance;
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