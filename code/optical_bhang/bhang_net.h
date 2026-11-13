#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Liveness-beacon cadence, in minutes. Used BOTH for the core1 timer period and
 * as the increment added to "total minutes monitored" per beacon, so they stay
 * consistent — change here only. */
#define BEACON_PERIOD_MIN 30u

/* Call once from main() after hw_init(). Connects WiFi and syncs NTP.
 * Blocks for up to ~25 s. Returns true if NTP synced.
 * Device continues operating (capture/analyse) even if this returns false. */
bool net_init(void);

/* True if WiFi is currently associated. */
bool net_wifi_connected(void);

/* Ensure the WiFi link is up, reconnecting (with backoff) if it has dropped or
 * the boot connect failed. Cheap when already connected. Call from the
 * networking core before/around uploads, and once per its main loop. Returns
 * true when associated with an IP. */
bool net_ensure_connected(void);

/* True once first NTP sync has succeeded. */
bool net_ntp_synced(void);

/* Write ISO 8601 UTC timestamp into buf (>= 24 bytes), or "null" if not synced. */
void net_fmt_utc(char *buf, size_t len);

/* Convert a boot-relative time_us_64() value to Unix epoch µs.
 * Returns false (and writes 0) until the first NTP sync. */
bool net_boot_to_epoch_us(uint64_t boot_us, uint64_t *epoch_us);

/* Read-modify-write the station status file on a detection event. Sets the two
 * decoded yields and raw timings and increments the record's lifetime
 * "total_captures"; a REAL detection also sets "nuke gone off?" = "yes" and
 * "nuke detected time". All other fields (uptime counter, etc.) are preserved.
 * If the existing record cannot be read (anything but "file absent"), nothing is
 * written and false is returned, so the caller can retry without data loss.
 * Blocks while it retries. Returns true on HTTP 200/201.
 * source         : "REAL" raises the alarm; anything else updates values only
 *                  (BIST results are never uploaded)
 * t0_boot_us     : flash onset as a time_us_64() value; written as the detected
 *                  time (null in JSON if the clock has never synced)
 * yield_tmin_kt  : yield from time-to-minima  (< 0 → null in JSON)
 * yield_tinter_kt: yield from inter-pulse interval (< 0 → null in JSON)
 * t_min_ms / t_inter_ms: also written into the status JSON */
bool net_upload_optical(const char *source,
                        uint64_t t0_boot_us,
                        float t_min_ms,
                        float t_inter_ms,
                        float yield_tmin_kt,
                        float yield_tinter_kt);

/* Liveness beacon: GET the station status file, read "total minutes monitored",
 * add BEACON_PERIOD_MIN, and PUT the updated status JSON back. Blocks for up to
 * ~25 s. Intended to be called once every BEACON_PERIOD_MIN minutes from the
 * networking core. Returns true on HTTP 200/201. */
bool net_upload_beacon(void);
