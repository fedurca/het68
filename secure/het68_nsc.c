// het68_nsc.c — Non-secure callable surface for NS → Secure entry.
//
// Dual-ELF builds (HET68_SAU_PROGRAM=1) mark these with cmse_nonsecure_entry so
// the linker emits an NSC veneer region. Lab single-image builds keep plain
// wrappers — cmse_nonsecure_entry on a monolithic ELF breaks pico-sdk link
// layouts (NSC section outside the FLASH segment).
#include "het68_secure.h"

#ifndef HET68_SAU_PROGRAM
#define HET68_SAU_PROGRAM 0
#endif

#if HET68_SAU_PROGRAM
#include <arm_cmse.h>
#define HET68_NSC_ATTR __attribute__((cmse_nonsecure_entry))
#else
#define HET68_NSC_ATTR
#endif

HET68_NSC_ATTR
void het68_nsc_plugin_status(het68_plugin_status_t *out) {
    if (!out) return;
#if HET68_SAU_PROGRAM && defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE & 1)
    if (cmse_check_address_range((void *)out, sizeof(*out),
                                 CMSE_NONSECURE | CMSE_MPU_READWRITE) == NULL)
        return;
#endif
    het68_plugin_status_get(out);
}

HET68_NSC_ATTR
bool het68_nsc_plugin_erase_slots(void) {
    return het68_plugin_erase_slots();
}

HET68_NSC_ATTR
bool het68_nsc_plugin_import_begin(void) {
    return het68_plugin_import_begin();
}

HET68_NSC_ATTR
bool het68_nsc_plugin_import_hex_line(const char *line) {
    if (!line) return false;
#if HET68_SAU_PROGRAM && defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE & 1)
    if (cmse_check_address_range((void *)line, 256,
                                 CMSE_NONSECURE | CMSE_MPU_READ) == NULL)
        return false;
#endif
    return het68_plugin_import_hex_line(line);
}

HET68_NSC_ATTR
bool het68_nsc_plugin_import_end(bool *programmed_out) {
#if HET68_SAU_PROGRAM && defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE & 1)
    if (programmed_out &&
        cmse_check_address_range((void *)programmed_out, sizeof(*programmed_out),
                                 CMSE_NONSECURE | CMSE_MPU_READWRITE) == NULL)
        return false;
#endif
    return het68_plugin_import_end(programmed_out);
}
