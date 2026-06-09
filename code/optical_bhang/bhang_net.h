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

/* Read-modify-write the station status file on a detection event. Always sets
 * the two decoded yields; a REAL detection also sets "nuke gone off?" = "yes"
 * and "nuke detected time" (a BIST is a self-test and does not raise the alarm).
 * All other fields (uptime counter, etc.) are preserved.
 * Blocks for up to ~25 s. Returns true on HTTP 200/201.
 * source         : "REAL" (raises alarm) or "BIST" (yields only)
 * yield_tmin_kt  : yield from time-to-minima  (< 0 → null in JSON)
 * yield_tinter_kt: yield from inter-pulse interval (< 0 → null in JSON)
 * t_min_ms / t_inter_ms / total_captures: ignored (kept for call compatibility) */
bool net_upload_optical(const char *source,
                        float t_min_ms,
                        float t_inter_ms,
                        float yield_tmin_kt,
                        float yield_tinter_kt,
                        uint32_t total_captures);

/* Liveness beacon: GET the station status file, read "total minutes monitored",
 * add BEACON_PERIOD_MIN, and PUT the updated status JSON back. Blocks for up to
 * ~25 s. Intended to be called once every BEACON_PERIOD_MIN minutes from the
 * networking core. Returns true on HTTP 200/201. */
bool net_upload_beacon(void);
