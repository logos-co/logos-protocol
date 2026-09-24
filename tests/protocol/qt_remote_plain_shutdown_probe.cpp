#include "implementations/qt_remote_plain/qt_remote_plain_transport.h"
#include "implementations/qt_remote_plain/qtro_transport.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "token_manager.h"

#include <QCoreApplication>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace logos::qt_remote_plain;

namespace {

class AdapterProvider final : public LogosProviderObject {
public:
    QVariant callMethod(const QString&, const QVariantList&) override { return true; }
    QJsonArray getMethods() override { return {}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("fixture"); }
    QString providerVersion() const override { return QStringLiteral("1"); }
    bool informModuleToken(const QString&, const QString&) override { return true; }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    AdapterProvider provider;
    TokenManager& tokens = TokenManager::instance();
    tokens.clearAllTokens();
    tokens.adoptCredential(QStringLiteral("secret"));
    ModuleProxy proxy(&provider, nullptr, &tokens);
    (void)proxy.saveToken(QStringLiteral("caller"), QStringLiteral("secret"));
    const QString url = QStringLiteral("local:qt_plain_shutdown_%1")
        .arg(QCoreApplication::applicationPid());
    auto host = std::make_unique<QtRemotePlainTransportHost>(url);
    if (!host->publishObject(QStringLiteral("fixture"), &proxy)) return 20;
    Client client;
    std::string error;
    if (!client.connect(url.toStdString(), 1s, &error)
        || !client.acquire("fixture", 1s, &error)) return 21;
    auto call = std::async(std::launch::async, [&] {
        return client.call("fixture", "callRemoteMethod(QString,QString,QVariantList)",
            {Variant::fromRpc(logos::plain::RpcValue{"secret"}),
             Variant::fromRpc(logos::plain::RpcValue{"echo"}),
             Variant::fromRpc(logos::plain::RpcValue{logos::plain::RpcList{}})},
            1s, &error);
    });
    std::this_thread::sleep_for(150ms);
    host.reset();
    if (call.wait_for(1s) != std::future_status::ready) return 22;
    (void)call.get();
    tokens.clearAllTokens();
    return 0;
}
