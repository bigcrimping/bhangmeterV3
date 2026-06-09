#pragma once
/**
 * bhang_config.h — runtime, flash-persisted device configuration.
 *
 * All credentials (WiFi, GitHub, NTP, device identity) live in a single struct
 * that is loaded from the last flash sector at boot and edited over serial with
 * SET/SAVE.  There is no compile-time secrets.h: when flash holds no valid
 * config (first boot / after CLEAR) the record is seeded blank and entered over
 * serial in config mode.
 */
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t magic;        /* CONFIG_MAGIC — marks a valid record            */
    uint32_t version;      /* schema version, bump when fields change        */
    char     wifi_ssid[33];
    char     wifi_pass[64];
    char     device_id[32];
    char     lat[16];
    char     lon[16];
    char     gh_user[40];
    char     gh_repo[40];
    char     gh_branch[32];
    char     gh_file[80];
    char     gh_token[128];
    char     ntp[64];
    uint32_t crc32;        /* CRC32 over every preceding byte                */
} bhang_config_t;

/* The single live configuration, read by bhang_net.c.  Defined in bhang_config.c. */
extern bhang_config_t g_cfg;

/* Load the persisted record from flash into g_cfg.
 * Returns true if a valid record (magic + version + CRC) was found. */
bool config_load(void);

/* Seed g_cfg with blank credentials plus a few non-secret operational defaults
 * (lat/lon "0.0", branch "main", the status filename, and an NTP server). Used
 * on first boot / after CLEAR before the user enters config over serial. */
void config_seed_defaults(void);

/* Persist g_cfg to flash.  Returns true on success.
 * The caller MUST guarantee detection is idle — a flash erase stalls XIP and
 * would corrupt a capture in progress. */
bool config_save(void);

/* Erase the persisted record (factory reset; takes effect on next boot). */
void config_clear(void);

/* Set one field by key (e.g. "wifi_ssid").  Value is copied verbatim (spaces ok).
 * Returns false for an unknown key. */
bool config_set(const char *key, const char *value);

/* Print every field to stdout, masking wifi_pass and gh_token. */
void config_show(void);

/* Tell the config module that core1 is running, so flash writes use the
 * multicore lockout path (flash_safe_execute) instead of a bare write. */
void config_set_dualcore(bool enabled);
