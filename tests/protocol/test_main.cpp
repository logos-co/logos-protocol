#include "logos_transport_config.h"

#include <QCoreApplication>
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Runs every test on the default local transport over qt_remote_plain, as
    // Windows does: LOGOS_TEST_LOCAL_TRANSPORT=qt_remote_plain.
    if (qgetenv("LOGOS_TEST_LOCAL_TRANSPORT") == "qt_remote_plain") {
        LogosTransportConfig plain;
        plain.protocol = LogosProtocol::QtRemotePlain;
        LogosTransportConfigGlobal::setDefault(plain);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
