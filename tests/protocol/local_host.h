#ifndef LOGOS_TESTS_LOCAL_HOST_H
#define LOGOS_TESTS_LOCAL_HOST_H

#include "logos_transport.h"
#include "logos_transport_config.h"
#include "logos_transport_factory.h"

#include <QString>

#include <memory>

// The host a consumer's local transport reaches, as the factory resolves it:
// qt_remote on Unix, qt_remote_plain on Windows or when that is the default.
inline std::unique_ptr<LogosTransportHost> makeLocalHost(const QString& url)
{
    return LogosTransportFactory::createHost(LogosTransportConfigGlobal::getDefault(), url);
}

// Whether the local transport resolves to qt_remote_plain here, as the factory decides.
inline bool localIsPlain()
{
#ifdef _WIN32
    return true;
#else
    return LogosTransportConfigGlobal::getDefault().protocol == LogosProtocol::QtRemotePlain;
#endif
}

#endif
