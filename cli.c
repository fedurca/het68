// cli.c — UART command-line interface for gallery + detection log + time/log.
#include "cli.h"
#include "debug_io.h"
#include "entity_store.h"
#include "detection_log.h"
#include "drone_store.h"
#include "het68_time.h"
#include "remote_id.h"
#include <string.h>

static char g_buf[192];
static uint32_t g_len;
static bool g_ent_import;
static bool g_det_import;
static bool g_help_shown;

void cli_print_help(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== het68 CLI ");
#ifdef HET68_VERSION_STR
    dbg_puts(HET68_VERSION_STR);
#else
    dbg_puts("?");
#endif
    dbg_puts(" ===\n");
    dbg_puts("HELP                 — this help\n");
    dbg_puts("STATUS               — all stored data + statistics\n");
    dbg_puts("TIME                 — show clock / sync status\n");
    dbg_puts("TIME INFO            — time source, age, quality\n");
    dbg_puts("TIME SYNC <unix>     — sync wall clock (required for DET timestamps)\n");
    dbg_puts("LOG ON | LOG OFF     — enable/disable SRC/ENTITY stdout logging\n");
    dbg_puts("LOG                  — show log status\n");
    dbg_puts("DET LIST             — list saved detections\n");
    dbg_puts("DET EXPORT           — NVR JSON Lines (NVREVT)\n");
    dbg_puts("DET BACKUP           — hex blob download (DETBLOB)\n");
    dbg_puts("DET IMPORT           — upload hex blob (DETHEX … / DET END)\n");
    dbg_puts("DET DEL <id>         — delete one detection\n");
    dbg_puts("DET CLEAR            — delete all detections\n");
    dbg_puts("ENT LIST             — entity gallery (classification templates)\n");
    dbg_puts("ENT EXPORT|IMPORT    — gallery hex blob transfer\n");
    dbg_puts("DRONE LIST           — known drones persisted in flash\n");
    dbg_puts("DRONE CLEAR          — erase known-drone flash registry\n");
    dbg_puts("RID LIST             — OpenDroneID BLE tracks (CYW43 boards)\n");
    dbg_puts("RID ON | RID OFF     — enable/disable BLE Remote ID scan\n");
    dbg_puts("Note: DET timestamps after TIME SYNC (UART or RID System msg).\n");
    dbg_puts("=================\n");
    dbg_line_unlock(lock);
}

void cli_print_status(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== STATUS ");
#ifdef HET68_VERSION_STR
    dbg_puts(HET68_VERSION_STR);
#else
    dbg_puts("?");
#endif
    dbg_puts(" ===\n");
    dbg_line_unlock(lock);

    het68_time_info_uart();

    lock = dbg_line_lock();
    dbg_puts("--- stats ---\n");
    dbg_puts("DET n=");
    dbg_putu32(detection_log_count());
    if (detection_log_dirty()) dbg_puts(" dirty");
    if (detection_log_saving()) dbg_puts(" saving");
    dbg_putc('\n');
    dbg_puts("ENT n=");
    dbg_putu32(entity_store_count());
    if (entity_store_dirty()) dbg_puts(" dirty");
    if (entity_store_saving()) dbg_puts(" saving");
    dbg_putc('\n');
    drone_store_stats_uart();
    dbg_puts("RID live=");
    dbg_putu32(remote_id_active_count());
    dbg_puts(" avail=");
    dbg_puts(remote_id_available() ? "1" : "0");
    dbg_puts(" scan=");
    dbg_puts(remote_id_enabled() ? "on" : "off");
    dbg_puts(" syncs=");
    dbg_putu32(remote_id_time_sync_count());
    dbg_putc('\n');
    dbg_puts("LOG stdout=");
    dbg_puts(dbg_log_enabled() ? "on" : "off");
    dbg_putc('\n');
    dbg_line_unlock(lock);

    entity_store_dump_uart();
    detection_log_list_uart();
    drone_store_list_uart();
    if (remote_id_available())
        remote_id_list_uart();

    lock = dbg_line_lock();
    dbg_puts("=== end STATUS ===\n");
    dbg_line_unlock(lock);
}

void cli_init(void) {
    g_len = 0;
    g_ent_import = false;
    g_det_import = false;
    g_help_shown = false;
}

void cli_on_connect(void) {
    if (g_help_shown) return;
    g_help_shown = true;
    cli_print_help();
}

static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static void handle_line(char *line) {
    // Trim trailing spaces
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

    if (g_ent_import) {
        if (strcmp(line, "ENT END") == 0 || strcmp(line, "ENTBLOB END") == 0) {
            entity_store_import_end();
            g_ent_import = false;
        } else if (!entity_store_import_hex_line(line)) {
            dbg_puts("ENT IMPORT ERR: bad hex line\n");
            g_ent_import = false;
        }
        return;
    }
    if (g_det_import) {
        if (strcmp(line, "DET END") == 0 || strcmp(line, "DETBLOB END") == 0) {
            detection_log_import_end();
            g_det_import = false;
        } else if (!detection_log_import_hex_line(line)) {
            dbg_puts("DET IMPORT ERR: bad hex line\n");
            g_det_import = false;
        }
        return;
    }

    if (line[0] == '\0' || strcmp(line, "HELP") == 0 || strcmp(line, "?") == 0) {
        cli_print_help();
        return;
    }

    if (strcmp(line, "STATUS") == 0) {
        cli_print_status();
        return;
    }

    if (strcmp(line, "TIME") == 0) {
        het68_time_dump_uart();
        return;
    }
    if (strcmp(line, "TIME INFO") == 0) {
        het68_time_info_uart();
        return;
    }
    if (strncmp(line, "TIME SYNC ", 10) == 0) {
        uint32_t ep = parse_u32(line + 10);
        if (het68_time_sync(ep)) {
            dbg_puts("TIME OK epoch=");
            dbg_putu32(het68_time_epoch_sec());
            dbg_puts(" source=uart quality=");
            dbg_putu32(het68_time_quality());
            dbg_putc('\n');
        } else {
            dbg_puts("TIME ERR: need unix epoch >= 1700000000\n");
        }
        return;
    }

    if (strcmp(line, "LOG") == 0) {
        dbg_puts("LOG stdout=");
        dbg_puts(dbg_log_enabled() ? "on" : "off");
        dbg_putc('\n');
        return;
    }
    if (strcmp(line, "LOG ON") == 0) {
        dbg_log_set(true);
        dbg_puts("LOG stdout=on\n");
        return;
    }
    if (strcmp(line, "LOG OFF") == 0) {
        dbg_log_set(false);
        dbg_puts("LOG stdout=off\n");
        return;
    }

    if (strcmp(line, "DET LIST") == 0 || strcmp(line, "DET DUMP") == 0) {
        detection_log_list_uart();
        return;
    }
    if (strcmp(line, "DET EXPORT") == 0) {
        detection_log_export_nvr();
        return;
    }
    if (strcmp(line, "DET BACKUP") == 0) {
        detection_log_export_hex();
        return;
    }
    if (strcmp(line, "DET IMPORT") == 0) {
        if (detection_log_import_begin()) {
            g_det_import = true;
            dbg_puts("DET IMPORT: send DETHEX lines, then DET END\n");
        } else {
            dbg_puts("DET IMPORT ERR: busy\n");
        }
        return;
    }
    if (strncmp(line, "DET DEL ", 8) == 0) {
        uint32_t id = parse_u32(line + 8);
        if (detection_log_delete_id(id)) dbg_puts("DET DEL OK\n");
        else dbg_puts("DET DEL ERR: not found\n");
        return;
    }
    if (strcmp(line, "DET CLEAR") == 0 || strcmp(line, "DET DELETE ALL") == 0) {
        detection_log_clear();
        dbg_puts("DET CLEAR OK\n");
        return;
    }

    if (strcmp(line, "ENT LIST") == 0 || strcmp(line, "ENT DUMP") == 0) {
        entity_store_dump_uart();
        return;
    }
    if (strcmp(line, "ENT EXPORT") == 0) {
        entity_store_export_uart();
        return;
    }
    if (strcmp(line, "ENT IMPORT") == 0) {
        if (entity_store_import_begin()) {
            g_ent_import = true;
            dbg_puts("ENT IMPORT: send ENTHEX lines, then ENT END\n");
        } else {
            dbg_puts("ENT IMPORT ERR: busy\n");
        }
        return;
    }
    if (strcmp(line, "ENT HELP") == 0) {
        cli_print_help();
        return;
    }

    if (strcmp(line, "DRONE LIST") == 0 || strcmp(line, "DRONE") == 0) {
        drone_store_list_uart();
        return;
    }
    if (strcmp(line, "DRONE CLEAR") == 0) {
        if (drone_store_clear()) dbg_puts("DRONE CLEAR OK (pending flash)\n");
        else dbg_puts("DRONE CLEAR ERR: busy\n");
        return;
    }

    if (strcmp(line, "RID LIST") == 0 || strcmp(line, "RID") == 0) {
        remote_id_list_uart();
        return;
    }
    if (strcmp(line, "RID ON") == 0) {
        if (!remote_id_available()) {
            dbg_puts("RID ERR: unsupported on this board\n");
        } else {
            remote_id_set_enabled(true);
            dbg_puts("RID scan=on\n");
        }
        return;
    }
    if (strcmp(line, "RID OFF") == 0) {
        if (!remote_id_available()) {
            dbg_puts("RID ERR: unsupported on this board\n");
        } else {
            remote_id_set_enabled(false);
            dbg_puts("RID scan=off\n");
        }
        return;
    }

    if (strncmp(line, "DET", 3) == 0 || strncmp(line, "ENT", 3) == 0 ||
        strncmp(line, "TIME", 4) == 0 || strncmp(line, "LOG", 3) == 0 ||
        strncmp(line, "RID", 3) == 0 || strncmp(line, "DRONE", 5) == 0 ||
        strncmp(line, "STATUS", 6) == 0) {
        dbg_puts("?: unknown command — HELP\n");
    }
}

void cli_rx_byte(int ch) {
    if (ch < 0) return;
    // First byte from host → treat as "connected" and show help once.
    if (!g_help_shown) cli_on_connect();

    if (ch == '\r') return;
    if (ch == '\n') {
        g_buf[g_len < sizeof(g_buf) ? g_len : (sizeof(g_buf) - 1u)] = '\0';
        handle_line(g_buf);
        g_len = 0;
        return;
    }
    if (g_len + 1u < sizeof(g_buf)) {
        g_buf[g_len++] = (char)ch;
    } else {
        g_len = 0;
    }
}
