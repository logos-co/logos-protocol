#ifndef LOGOS_MODULE_CONFIGURATION_H
#define LOGOS_MODULE_CONFIGURATION_H

/* Module configuration: one JSON document per module, delivered by the host on every
 * start, before the context is set and before the module is published. It never
 * carries authority.
 *
 * OPTIONAL module export (generated glue), looked up by name like the runtime delegate.
 * Returns 0 to accept; it calls no lp_* and refuses JSON it cannot parse. A host with a
 * document for an image without the export fails the load. */
#define LOGOS_MODULE_SET_CONFIGURATION_SYMBOL "logos_module_set_configuration"
typedef int (*logos_module_set_configuration_fn)(const char* configuration_json);

#endif /* LOGOS_MODULE_CONFIGURATION_H */
