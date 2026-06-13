/**
 * bhang_config.c — flash-persisted device configuration.
 *
 * The record is stored in the last 4 KB sector of flash.  config_save() writes
 * it either with a bare interrupt-disabled flash op (single core, during
 * first-boot config mode) or via flash_safe_execute() (dual core, once core1 is
 * up and registered as a lockout victim).
 */
#include "bhang_config.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#define CONFIG_MAGIC        0xB4A06C01u
#define CONFIG_VERSION      1u
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_XIP_ADDR     (XIP_BASE + CONFIG_FLASH_OFFSET)

bhang_config_t g_cfg;

static volatile bool g_dualcore = false;

/* One scratch sector, prepared then committed by the flash writer callback. */
static uint8_t s_pagebuf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

/* ---------------------------------------------------------------------------
 * CRC32 (bitwise, poly 0xEDB88320) — small and dependency-free
 * --------------------------------------------------------------------------- */
static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

static uint32_t config_crc(const bhang_config_t *c) {
    return crc32_calc((const uint8_t *)c, offsetof(bhang_config_t, crc32));
}

/* ---------------------------------------------------------------------------
 * Field table for SET / SHOW
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *key;
    size_t      off;
    size_t      size;
    bool        secret;
} cfg_field_t;

#define CFG_FIELD(name, sec) \
    { #name, offsetof(bhang_config_t, name), sizeof(((bhang_config_t *)0)->name), (sec) }

static const cfg_field_t FIELDS[] = {
    CFG_FIELD(wifi_ssid, false),
    CFG_FIELD(wifi_pass, true),
    CFG_FIELD(device_id, false),
    CFG_FIELD(lat,       false),
    CFG_FIELD(lon,       false),
    CFG_FIELD(gh_user,   false),
    CFG_FIELD(gh_repo,   false),
    CFG_FIELD(gh_branch, false),
    CFG_FIELD(gh_file,   false),
    CFG_FIELD(gh_token,  true),
    CFG_FIELD(ntp,       false),
};
#define NUM_FIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

/* ---------------------------------------------------------------------------
 * Load / seed
 * --------------------------------------------------------------------------- */
bool config_load(void) {
    const bhang_config_t *f = (const bhang_config_t *)(CONFIG_XIP_ADDR);
    if (f->magic != CONFIG_MAGIC)     return false;
    if (f->version != CONFIG_VERSION) return false;
    if (config_crc(f) != f->crc32)    return false;
    memcpy(&g_cfg, f, sizeof(g_cfg));
    return true;
}

void config_seed_defaults(void) {
    /* All credentials/identity start blank — they are entered over serial in
     * first-boot config mode (SET ... then SAVE). Only a few non-secret
     * operational fields get sensible defaults; lat/lon default to "0.0" so the
     * status JSON stays valid before the user sets them. */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic   = CONFIG_MAGIC;
    g_cfg.version = CONFIG_VERSION;
    snprintf(g_cfg.lat,       sizeof(g_cfg.lat),       "%s", "0.0");
    snprintf(g_cfg.lon,       sizeof(g_cfg.lon),       "%s", "0.0");
    snprintf(g_cfg.gh_branch, sizeof(g_cfg.gh_branch), "%s", "main");
    snprintf(g_cfg.gh_file,   sizeof(g_cfg.gh_file),   "%s", "events_optical.json");
    snprintf(g_cfg.ntp,       sizeof(g_cfg.ntp),       "%s", "pool.ntp.org");
}

/* ---------------------------------------------------------------------------
 * Flash write paths
 * --------------------------------------------------------------------------- */
static void config_flash_writer(void *param) {
    (void)param;
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, s_pagebuf, FLASH_SECTOR_SIZE);
}

static void config_flash_eraser(void *param) {
    (void)param;
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
}

/* Run a flash op safely: via multicore lockout when core1 is up, otherwise a
 * bare interrupt-disabled write (single core has no other XIP executor). */
static bool config_run_flash_op(void (*op)(void *)) {
    if (g_dualcore) {
        int rc = flash_safe_execute(op, NULL, 2000);
        if (rc != PICO_OK) {
            printf("CONFIG: flash_safe_execute failed rc=%d\r\n", rc);
            return false;
        }
        return true;
    }
    uint32_t ints = save_and_disable_interrupts();
    op(NULL);
    restore_interrupts(ints);
    return true;
}

bool config_save(void) {
    g_cfg.magic   = CONFIG_MAGIC;
    g_cfg.version = CONFIG_VERSION;
    g_cfg.crc32   = config_crc(&g_cfg);

    memset(s_pagebuf, 0xFF, sizeof(s_pagebuf));
    memcpy(s_pagebuf, &g_cfg, sizeof(g_cfg));
    return config_run_flash_op(config_flash_writer);
}

void config_clear(void) {
    config_run_flash_op(config_flash_eraser);
}

/* ---------------------------------------------------------------------------
 * SET / SHOW
 * --------------------------------------------------------------------------- */
bool config_set(const char *key, const char *value) {
    for (size_t i = 0; i < NUM_FIELDS; i++) {
        if (strcmp(key, FIELDS[i].key) == 0) {
            char *dst = (char *)&g_cfg + FIELDS[i].off;
            snprintf(dst, FIELDS[i].size, "%s", value);
            return true;
        }
    }
    return false;
}

static void print_masked(const char *v) {
    size_t n = strlen(v);
    if (n == 0)      printf("(empty)");
    else if (n <= 4) printf("****");
    else             printf("%c%c...%c%c", v[0], v[1], v[n - 2], v[n - 1]);
}

void config_show(void) {
    for (size_t i = 0; i < NUM_FIELDS; i++) {
        const char *v = (const char *)&g_cfg + FIELDS[i].off;
        printf("  %-10s = ", FIELDS[i].key);
        if (FIELDS[i].secret) print_masked(v);
        else                  printf("%s", v);
        printf("\r\n");
    }
}

void config_set_dualcore(bool enabled) {
    g_dualcore = enabled;
}
