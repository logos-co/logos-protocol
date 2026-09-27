#ifndef LOGOS_RUNTIME_DELEGATE_H
#define LOGOS_RUNTIME_DELEGATE_H

/* Runtime delegate (0.13): a module image loaded in a runtime host's process runs
 * its outbound client calls in the host's runtime, as the identity the host
 * admitted it as (the origin it passes is ignored). Forwarded: clients, calls,
 * subscriptions, introspection, token pushes, and (0.14) a client's tls_tcp
 * session credential and hook. Image-local: versions, strings, modes, the default
 * transport, the caller document, providers and lp_token_*.
 * Strings a host entry returns are copied into the image and freed by the host.
 * The table is sized: entries past string_free are used only when `size` covers
 * them, and a host without the session entries refuses them, as before 0.14. */

#include "logos_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LP_RUNTIME_DELEGATE_VERSION 1

typedef struct lp_runtime_delegate_v1 {
    unsigned size;    /* sizeof(lp_runtime_delegate_v1) as the host built it */
    unsigned version; /* LP_RUNTIME_DELEGATE_VERSION */
    void* context;    /* the host's per-identity state; opaque to the image */

    lp_client* (*client_create)(void* context, const char* target_module,
                                const char* target_transport_json,
                                const char* capability_transport_json);
    void (*client_destroy)(void* context, lp_client* client);
    int (*invoke)(void* context, lp_client* client, const char* method,
                  const char* args_json, int timeout_ms,
                  char** out_result_json, char** out_error_json);
    int (*invoke_async)(void* context, lp_client* client, const char* method,
                        const char* args_json, int timeout_ms,
                        lp_result_cb cb, void* user_data);
    lp_subscription* (*subscribe)(void* context, lp_client* client,
                                  const char* event_name, lp_event_cb cb,
                                  void* user_data);
    void (*unsubscribe)(void* context, lp_subscription* subscription);
    int (*set_subscription_status_cb)(void* context, lp_client* client,
                                      lp_subscription_status_cb cb, void* user_data);
    unsigned long long (*subscription_generation)(void* context, lp_client* client);
    int (*set_subscription_options)(void* context, lp_client* client,
                                    const char* options_json);
    int (*rearm_subscriptions)(void* context, lp_client* client);
    char* (*pending_subscriptions)(void* context, lp_client* client);
    char* (*get_methods)(void* context, lp_client* client);
    int (*inform_module_token)(void* context, lp_client* client, const char* auth_token,
                               const char* module_name, const char* token);
    int (*inform_module_token_to)(void* context, lp_client* client, const char* auth_token,
                                  const char* origin_module, const char* module_name,
                                  const char* token, int timeout_ms);
    int (*revoke_module_token_to)(void* context, lp_client* client, const char* auth_token,
                                  const char* origin_module, const char* module_name,
                                  const char* token_digest, int timeout_ms);
    void (*string_free)(void* context, char* value);

    /* 0.14: tls_tcp sessions, as lp_client_set_tls_credential and
     * lp_client_set_session_hook; the hooks run on the host's threads. */
    int (*client_set_tls_credential)(void* context, lp_client* client,
                                     const char* cert_chain_pem, const char* key_pem);
    int (*client_set_session_hook)(void* context, lp_client* client, lp_session_hook_cb dial,
                                   lp_session_hook_cb hello, void* user_data);
} lp_runtime_delegate_v1;

/* HOST SIDE (shared runtime). NULL unless `identity` is isolated and holds its
 * credential, so a module never runs on the host's store. `grants_json` is the
 * identity's host-service grant (as lp_grant_host_services), for its calls only. */
LP_API const lp_runtime_delegate_v1* lp_runtime_delegate_create(const char* identity,
                                                                const char* grants_json);

/* Destroys the identity's clients and subscriptions; later calls through the
 * table fail (LP_ERR_UNAVAILABLE or NULL). The table itself is never freed. */
LP_API void lp_runtime_delegate_release(const lp_runtime_delegate_v1* delegate);

/* IMAGE SIDE (static runtime). Installs `delegate` once: LP_ERR_UNSUPPORTED
 * after this image created a client of its own, when already installed, and
 * always in the shared runtime. */
int lp_runtime_install_delegate(const lp_runtime_delegate_v1* delegate);

/* OPTIONAL module export (generated glue): a host passes the delegate through it
 * before initialization; an image without it is not loaded in-process. */
#define LOGOS_MODULE_SET_RUNTIME_DELEGATE_SYMBOL "logos_module_set_runtime_delegate"
typedef int (*logos_module_set_runtime_delegate_fn)(const lp_runtime_delegate_v1* delegate);

#ifdef __cplusplus
}
#endif

#endif /* LOGOS_RUNTIME_DELEGATE_H */
