#ifndef FACE_TSS_CONFIGURATION_H
#define FACE_TSS_CONFIGURATION_H

/* FACE Configuration interface injection (issue #3).
 *
 * Per the FACE Technical Standard (3.1):
 * - FACE::CONFIGURATION_RESOURCE is a bounded string (STRING_TYPE, 256)
 *   naming the location of the configuration resource for
 *   TSS::Base::Initialize: "a file name reference, or a reference to a
 *   configuration service".
 * - FACE::TSS::Base::Set_Reference (the Injectable pattern) installs the
 *   Configuration interface reference; "a new Base instance must set its
 *   reference to a Configuration Interface before the Base is initialized."
 *
 * This implementation keeps JSON configuration as a documented convenience
 * adapter: when no Configuration interface is injected, the resource is
 * resolved by the built-in JSON loader - "json:{...}" for inline JSON,
 * otherwise a file path.
 */

#include "face_tss/types.h"
#include "face_tss/config.h"

/* Forward declaration (tss.h also declares it; C11 allows the repeat). */
typedef struct FACE_TSS FACE_TSS;

#ifdef __cplusplus
extern "C" {
#endif

/* FACE::CONFIGURATION_RESOURCE: bounded string (STRING_TYPE bound 256)
 * locating the configuration resource. */
#define FACE_TSS_CONFIGURATION_RESOURCE_MAX 256

/* FACE::Configuration interface provider (Injectable). The TSS calls
 * load() during Initialize with the resource named at Initialize time;
 * load() fills *config or returns an error code. The struct is copied by
 * Set_Reference; `user` is passed through untouched. */
typedef struct FACE_TSS_CONFIGURATION {
    FACE_TSS_RETURN_CODE (*load)(const char *resource,
                                 FACE_TSS_CONFIG *config, void *user);
    void *user;
} FACE_TSS_CONFIGURATION;

/* The only interface name this TSS accepts via Set_Reference. */
#define FACE_TSS_CONFIGURATION_INTERFACE_NAME "Configuration"

/* FACE::TSS::Base::Set_Reference (Injectable). Installs the Configuration
 * interface reference; must be called before Initialize.
 * - NO_ERROR: reference stored.
 * - NO_ACTION: the same reference (same load fn + user) is already set.
 * - NOT_AVAILABLE: a different Configuration reference is already set
 *   (one per TSS instance).
 * - INVALID_MODE: the TSS is already initialized (steady state).
 * - INVALID_PARAM: NULL tss/configuration, or an interface_name other
 *   than "Configuration". */
FACE_TSS_RETURN_CODE face_tss_set_reference(
    FACE_TSS *tss, const char *interface_name,
    const FACE_TSS_CONFIGURATION *configuration,
    FACE_TSS_UID_TYPE id);

/* FACE::TSS::Base::Initialize(CONFIGURATION_RESOURCE). Resolves the
 * resource through the injected Configuration interface when one was set
 * via Set_Reference, otherwise through the built-in JSON adapter
 * ("json:{...}" inline, or a file path). Idempotent: a second call
 * returns NO_ACTION. */
FACE_TSS_RETURN_CODE face_tss_initialize_from_resource(
    FACE_TSS *tss, const char *configuration_resource);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_CONFIGURATION_H */
