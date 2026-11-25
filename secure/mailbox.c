// mailbox.c — NS-visible mirror of Secure detection results.
//
// With builtin detection (default) Core1 still writes the existing g_doa_*
// volatiles directly. When HET68_PLUGIN_EXTERNAL is enabled, the Secure plugin
// should publish through het68_mailbox_publish() so NS CLI/USB never touch
// Secure BSS.
#include "het68_secure.h"
#include "doa.h"

#include <string.h>

typedef struct {
    uint32_t out;
    uint32_t nactive;
    uint32_t iter;
    uint32_t ndrone;
    uint32_t nwalker;
    uint32_t nvehicle;
    uint32_t nbird;
    uint32_t nspeech;
    uint32_t entity_id;
    uint32_t wind;
    float wind_az;
    float wind_el;
    float wind_db;
} het68_mailbox_t;

static volatile het68_mailbox_t g_mbox;

void het68_mailbox_publish(void) {
    // Snapshot from Secure-side symbols (same storage in builtin mode).
    g_mbox.out = g_doa_out;
    g_mbox.nactive = g_doa_nactive;
    g_mbox.iter = g_doa_iter;
    g_mbox.ndrone = g_doa_ndrone;
    g_mbox.nwalker = g_doa_nwalker;
    g_mbox.nvehicle = g_doa_nvehicle;
    g_mbox.nbird = g_doa_nbird;
    g_mbox.nspeech = g_doa_nspeech;
    g_mbox.entity_id = g_doa_entity_id;
    g_mbox.wind = g_doa_wind;
    g_mbox.wind_az = g_doa_wind_az;
    g_mbox.wind_el = g_doa_wind_el;
    g_mbox.wind_db = g_doa_wind_db;
}

void het68_mailbox_pull_to_ns(void) {
#if HET68_PLUGIN_EXTERNAL
    g_doa_out = g_mbox.out;
    g_doa_nactive = g_mbox.nactive;
    g_doa_iter = g_mbox.iter;
    g_doa_ndrone = g_mbox.ndrone;
    g_doa_nwalker = g_mbox.nwalker;
    g_doa_nvehicle = g_mbox.nvehicle;
    g_doa_nbird = g_mbox.nbird;
    g_doa_nspeech = g_mbox.nspeech;
    g_doa_entity_id = g_mbox.entity_id;
    g_doa_wind = g_mbox.wind;
    g_doa_wind_az = g_mbox.wind_az;
    g_doa_wind_el = g_mbox.wind_el;
    g_doa_wind_db = g_mbox.wind_db;
#else
    // Builtin: g_doa_* already NS-visible.
#endif
}
