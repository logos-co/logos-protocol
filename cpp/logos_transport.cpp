#include "logos_transport.h"

#include "logos_instance.h"

// Default URL generators for the Qt local-socket / QRO-based backends.
// They match today's deterministic scheme in LogosInstance::id(moduleName).

QString LogosTransportHost::bindUrl(const QString& /*instanceId*/,
                                    const QString& moduleName)
{
    return LogosInstance::id(moduleName);
}

QString LogosTransportConnection::endpointUrl(const QString& /*instanceId*/,
                                              const QString& moduleName)
{
    return LogosInstance::id(moduleName);
}
