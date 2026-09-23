// Which implementation a transport resolves to. On Windows a local transport
// is the plain one for hosts and connections alike: Qt's own QtRO never pairs
// with the plain core there, and nothing falls back to it.

#include "logos_mode.h"
#include "logos_transport_config.h"
#include "logos_transport_factory.h"
#include "implementations/qt_remote/remote_transport.h"
#include "implementations/qt_remote_plain/qt_remote_plain_transport.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>

#include <atomic>

using logos::qt_remote_plain::QtRemotePlainTransportConnection;
using logos::qt_remote_plain::QtRemotePlainTransportHost;

namespace {

QString uniqueUrl()
{
    static std::atomic<unsigned> serial{0};
    return QStringLiteral("local:logos_factory_%1_%2")
        .arg(QCoreApplication::applicationPid())
        .arg(serial.fetch_add(1));
}

constexpr bool kPlainOnly =
#ifdef _WIN32
    true;
#else
    false;
#endif

template <typename T, typename U>
bool is(const U& pointer)
{
    return dynamic_cast<T*>(pointer.get()) != nullptr;
}

} // namespace

TEST(TransportResolution, ALocalTransportResolvesToThePlatformsImplementation)
{
    const LogosTransportConfig local; // LogosProtocol::LocalSocket
    const QString url = uniqueUrl();
    const auto host = LogosTransportFactory::createHost(local, url);
    const auto connection = LogosTransportFactory::createConnection(local, url);
    ASSERT_TRUE(host && connection);
    EXPECT_EQ(is<QtRemotePlainTransportHost>(host), kPlainOnly);
    EXPECT_EQ(is<RemoteTransportHost>(host), !kPlainOnly);
    EXPECT_EQ(is<QtRemotePlainTransportConnection>(connection), kPlainOnly);
    EXPECT_EQ(is<RemoteTransportConnection>(connection), !kPlainOnly);
    EXPECT_EQ(LogosTransportFactory::needsQtEventLoop(local), !kPlainOnly);
}

TEST(TransportResolution, TheProcessDefaultFollowsTheSameRule)
{
    // No per-instance config: what a Qt host uses when the daemon sent none.
    const auto host = LogosTransportFactory::createHost(uniqueUrl());
    const auto connection = LogosTransportFactory::createConnection(uniqueUrl());
    ASSERT_TRUE(host && connection);
    EXPECT_EQ(is<QtRemotePlainTransportHost>(host), kPlainOnly);
    EXPECT_EQ(is<QtRemotePlainTransportConnection>(connection), kPlainOnly);
}

TEST(TransportResolution, QtRemotePlainIsPlainEverywhere)
{
    LogosTransportConfig plain;
    plain.protocol = LogosProtocol::QtRemotePlain;
    const auto host = LogosTransportFactory::createHost(plain, uniqueUrl());
    EXPECT_TRUE(is<QtRemotePlainTransportHost>(host));
    EXPECT_FALSE(LogosTransportFactory::needsQtEventLoop(plain));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    LogosModeConfig::setMode(LogosMode::Remote);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
