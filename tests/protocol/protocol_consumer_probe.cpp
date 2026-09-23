// Built twice: once linked against each shared protocol library. Reports
// which library this image's own reference to lp_client_create bound to.
#include "logos_protocol.h"

#include <dlfcn.h>

extern "C" const char* PROBE_FUNCTION()
{
    Dl_info info{};
    void* target = reinterpret_cast<void*>(&lp_client_create);
    return dladdr(target, &info) && info.dli_fname ? info.dli_fname : "";
}
