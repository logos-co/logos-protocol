#include "mock_store.h"
#include "../../token_manager.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMutexLocker>
#include <QtGlobal>

namespace {

// Seed this image's MockStore from LOGOS_MOCK_FIXTURE, if set.
//
// Self-service because the static is per-image: a plugin that links
// logos-protocol statically carries its own MockStore and TokenManager, so a
// host can only ever seed its own copy and the plugin would read an empty one.
// Every copy reads the fixture instead. LogosModeConfig::getMode() does the same.
//
// The file must be FULLY RESOLVED — this runs in images that have no idea where
// an application installed anything.
//
// Keys are "<module>.<wireMethod>"; keys starting with '_' are comments.
void seedFromFixtureIfRequested(MockStore& store)
{
    const QByteArray path = qgetenv("LOGOS_MOCK_FIXTURE");
    if (path.isEmpty()) return;

    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "MockStore: LOGOS_MOCK_FIXTURE is set to" << path
                   << "but could not be opened — this image will answer every"
                      " call with an invalid QVariant";
        return;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "MockStore: could not parse LOGOS_MOCK_FIXTURE" << path
                   << "-" << err.errorString();
        return;
    }

    const QJsonObject calls = doc.object().value(QStringLiteral("calls")).toObject();
    int loaded = 0;
    for (auto it = calls.constBegin(); it != calls.constEnd(); ++it) {
        const QString key = it.key();
        if (key.startsWith(QLatin1Char('_'))) continue;
        const int dot = key.indexOf(QLatin1Char('.'));
        if (dot <= 0 || dot == key.size() - 1) continue;

        const QString module = key.left(dot);
        store.when(module, key.mid(dot + 1)).thenReturn(it.value().toVariant());
        // A non-empty token stops LogosAPIClient dialling capability_module and
        // waiting out its 20s timeout on the first call. Per-image too.
        TokenManager::instance().saveToken(module,
                                           QStringLiteral("mock-token-") + module);
        ++loaded;
    }

    qInfo().noquote() << QStringLiteral(
        "MockStore: seeded %1 canned call(s) from %2")
        .arg(loaded).arg(QString::fromUtf8(path));
}

} // namespace

MockStore& MockStore::instance()
{
    static MockStore s;
    static const bool seeded = [&] { seedFromFixtureIfRequested(s); return true; }();
    Q_UNUSED(seeded);
    return s;
}

void MockStore::reset()
{
    QMutexLocker lock(&m_mutex);
    m_expectations.clear();
    m_calls.clear();
    m_mockObjectReleaseProbe = nullptr;
}

void MockStore::setMockObjectReleaseProbe(std::atomic<int>* counter)
{
    QMutexLocker lock(&m_mutex);
    m_mockObjectReleaseProbe = counter;
}

void MockStore::incrementMockObjectReleaseProbeIfSet()
{
    QMutexLocker lock(&m_mutex);
    if (m_mockObjectReleaseProbe)
        ++(*m_mockObjectReleaseProbe);
}

// ── ExpectationBuilder ───────────────────────────────────────────────────────

MockStore::ExpectationBuilder::ExpectationBuilder(MockStore& store,
                                                  const QString& module,
                                                  const QString& method)
    : m_store(store)
{
    QMutexLocker lock(&store.m_mutex);
    MockExpectation exp;
    exp.module = module;
    exp.method = method;
    exp.matchAnyArgs = true;
    store.m_expectations.append(exp);
    m_index = store.m_expectations.size() - 1;
}

MockStore::ExpectationBuilder& MockStore::ExpectationBuilder::withArgs(const QVariantList& args)
{
    QMutexLocker lock(&m_store.m_mutex);
    m_store.m_expectations[m_index].expectedArgs = args;
    m_store.m_expectations[m_index].matchAnyArgs = false;
    return *this;
}

MockStore::ExpectationBuilder& MockStore::ExpectationBuilder::thenReturn(const QVariant& value)
{
    QMutexLocker lock(&m_store.m_mutex);
    m_store.m_expectations[m_index].returnValue = value;
    return *this;
}

// ── MockStore ────────────────────────────────────────────────────────────────

MockStore::ExpectationBuilder MockStore::when(const QString& module, const QString& method)
{
    return ExpectationBuilder(*this, module, method);
}

QVariant MockStore::recordAndReturn(const QString& module, const QString& method,
                                    const QVariantList& args)
{
    QMutexLocker lock(&m_mutex);

    MockCallRecord record;
    record.module = module;
    record.method = method;
    record.args   = args;
    m_calls.append(record);

    // Search expectations in reverse (last registered wins)
    for (int i = m_expectations.size() - 1; i >= 0; --i) {
        const MockExpectation& exp = m_expectations.at(i);
        if (exp.module != module || exp.method != method) continue;
        if (!exp.matchAnyArgs && exp.expectedArgs != args) continue;
        qDebug() << "MockStore: matched expectation for" << module << "::" << method
                 << "-> returning" << exp.returnValue;
        return exp.returnValue;
    }

    qWarning() << "MockStore: no expectation registered for" << module << "::" << method
               << "- returning invalid QVariant";
    return QVariant();
}

bool MockStore::wasCalled(const QString& module, const QString& method) const
{
    QMutexLocker lock(&m_mutex);
    for (const MockCallRecord& r : m_calls) {
        if (r.module == module && r.method == method) return true;
    }
    return false;
}

bool MockStore::wasCalledWith(const QString& module, const QString& method,
                              const QVariantList& args) const
{
    QMutexLocker lock(&m_mutex);
    for (const MockCallRecord& r : m_calls) {
        if (r.module == module && r.method == method && r.args == args) return true;
    }
    return false;
}

int MockStore::callCount(const QString& module, const QString& method) const
{
    QMutexLocker lock(&m_mutex);
    int count = 0;
    for (const MockCallRecord& r : m_calls) {
        if (r.module == module && r.method == method) ++count;
    }
    return count;
}

QVariantList MockStore::lastArgs(const QString& module, const QString& method) const
{
    QMutexLocker lock(&m_mutex);
    for (int i = m_calls.size() - 1; i >= 0; --i) {
        const MockCallRecord& r = m_calls.at(i);
        if (r.module == module && r.method == method) return r.args;
    }
    return QVariantList();
}

QList<MockCallRecord> MockStore::allCalls() const
{
    QMutexLocker lock(&m_mutex);
    return m_calls;
}
