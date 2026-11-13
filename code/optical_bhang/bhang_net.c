/**
 * bhang_net.c — WiFi init, NTP sync, GitHub Contents API upload.
 *
 * net_init() runs once on core0 at boot (cyw43 must be initialised on the core
 * that owns the GPIO interrupts). The blocking upload calls — net_upload_optical()
 * and net_upload_beacon(), each a read-modify-write of the status file — are
 * made from core1 only; they share one static HTTP client and are not re-entrant.
 */

#include "bhang_net.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/sntp.h"

#include "bhang_config.h"   /* all credentials come from g_cfg at runtime */
#include "led.h"

/* WIFI_COUNTRY is a numeric SDK enum (not a runtime string) — compile-time only.
 * Override via a build define if you need a different region. */
#ifndef WIFI_COUNTRY
#  define WIFI_COUNTRY CYW43_COUNTRY_UK
#endif

#define FIRMWARE_VERSION "optical_bhang_v1"

/* ---------------------------------------------------------------------------
 * Wall-clock state (written from SNTP callback)
 * --------------------------------------------------------------------------- */
static volatile bool     g_ntp_synced   = false;
static volatile uint64_t g_ntp_epoch_us = 0;
static volatile uint64_t g_ntp_boot_us  = 0;

/* Called via SNTP_SET_SYSTEM_TIME_US macro in lwipopts.h */
void sntp_set_time_callback(uint32_t sec, uint32_t us) {
    g_ntp_epoch_us = (uint64_t)sec * 1000000ULL + (uint64_t)us;
    g_ntp_boot_us  = time_us_64();
    g_ntp_synced   = true;
    /* No printf here: this runs in the cyw43 background (interrupt) context. */
}

/* ---------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------- */
static bool g_net_inited     = false;
static bool g_wifi_connected = false;
static bool g_sntp_started   = false;

/* Runtime auto-reconnect backoff (used by net_ensure_connected on core1). */
static absolute_time_t g_next_reconnect;            /* zero-init → first try is immediate */
static uint32_t        g_reconnect_backoff_ms = 0;

/* ---------------------------------------------------------------------------
 * HTTPS client helpers
 * --------------------------------------------------------------------------- */
#define HTTP_BUF_SIZE 8192

typedef struct {
    struct altcp_pcb *pcb;
    char      req[HTTP_BUF_SIZE];
    size_t    req_len;
    char      resp[HTTP_BUF_SIZE];
    size_t    resp_len;
    bool      done;
    int       status_code;
    bool      doing_get;
    char      sha[64];
    bool      sha_ready;
    ip_addr_t server_addr;
    bool      dns_done;
    bool      dns_ok;
} gh_client_t;

/* One shared ~16 KB HTTP client buffer. Networking is single-threaded (core1
 * only) and every request completes before the next starts, so a single static
 * instance keeps it off the 4 KB core stack without three copies in RAM. */
static gh_client_t g_http;

static err_t gh_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    gh_client_t *c = (gh_client_t *)arg;
    (void)err;
    if (!p) { c->done = true; return ERR_OK; }

    size_t copy = p->tot_len;
    if (c->resp_len + copy >= HTTP_BUF_SIZE - 1)
        copy = HTTP_BUF_SIZE - 1 - c->resp_len;
    pbuf_copy_partial(p, c->resp + c->resp_len, (uint16_t)copy, 0);
    c->resp_len += copy;
    c->resp[c->resp_len] = '\0';
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (c->status_code == 0 && c->resp_len > 12)
        c->status_code = atoi(c->resp + 9);  /* "HTTP/1.1 XXX" */

    if (c->doing_get && !c->sha_ready) {
        char *pos = strstr(c->resp, "\"sha\":\"");
        if (pos) {
            pos += 7;
            char *end = strchr(pos, '"');
            if (end && (size_t)(end - pos) < sizeof(c->sha)) {
                size_t n = (size_t)(end - pos);
                memcpy(c->sha, pos, n);
                c->sha[n]    = '\0';
                c->sha_ready = true;
            }
        }
    }
    return ERR_OK;
}

static err_t gh_sent(void *arg, struct altcp_pcb *pcb, uint16_t len) {
    (void)arg; (void)pcb; (void)len;
    return ERR_OK;
}

static void gh_err(void *arg, err_t err) {
    gh_client_t *c = (gh_client_t *)arg;
    (void)err;
    /* On a fatal error lwIP has already freed the whole altcp/TLS chain (and its
     * mbedTLS state) before calling us — NULL the pcb so gh_run()'s cleanup does
     * not close/abort freed memory. */
    c->pcb  = NULL;
    c->done = true;
}

static err_t gh_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    gh_client_t *c = (gh_client_t *)arg;
    if (err != ERR_OK) { c->done = true; return err; }
    altcp_write(pcb, c->req, c->req_len, TCP_WRITE_FLAG_COPY);
    altcp_output(pcb);
    return ERR_OK;
}

static void gh_dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    gh_client_t *c = (gh_client_t *)arg;
    (void)name;
    if (addr) { c->server_addr = *addr; c->dns_ok = true; }
    c->dns_done = true;
}

/* Read a bool flag written by an lwIP callback (which runs in the cyw43 context
 * on core0) from this (caller's) core, under the lwIP lock so the read is a
 * proper cross-core synchronisation point. */
static bool gh_flag(volatile bool *flag) {
    cyw43_arch_lwip_begin();
    bool v = *flag;
    cyw43_arch_lwip_end();
    return v;
}

static bool gh_run(gh_client_t *c, struct altcp_tls_config *tls,
                   uint32_t timeout_ms) {
    c->dns_done = false; c->dns_ok = false;
    cyw43_arch_lwip_begin();
    err_t de = dns_gethostbyname("api.github.com", &c->server_addr, gh_dns_cb, c);
    cyw43_arch_lwip_end();
    if (de == ERR_OK) {
        c->dns_ok = c->dns_done = true;
    } else if (de == ERR_INPROGRESS) {
        uint64_t dl = time_us_64() + 5000000ULL;
        while (!gh_flag(&c->dns_done) && time_us_64() < dl) {
            sleep_ms(1);
        }
    }
    if (!c->dns_ok) { printf("NET:   DNS failed for api.github.com\r\n"); return false; }
    printf("NET:   DNS ok -> %s\r\n", ipaddr_ntoa(&c->server_addr));

    cyw43_arch_lwip_begin();
    c->pcb = altcp_tls_new(tls, IPADDR_TYPE_V4);
    if (c->pcb) {
        altcp_arg(c->pcb, c);
        altcp_recv(c->pcb, gh_recv);
        altcp_sent(c->pcb, gh_sent);
        altcp_err(c->pcb,  gh_err);
    }
    cyw43_arch_lwip_end();
    if (!c->pcb) { printf("NET:   altcp_tls_new failed (out of memory?)\r\n"); return false; }

    cyw43_arch_lwip_begin();
    err_t ce = altcp_connect(c->pcb, &c->server_addr, 443, gh_connected);
    cyw43_arch_lwip_end();
    if (ce != ERR_OK && ce != ERR_INPROGRESS) {
        printf("NET:   altcp_connect failed err=%d\r\n", ce);
    } else {
        uint64_t dl = time_us_64() + (uint64_t)timeout_ms * 1000u;
        while (!gh_flag(&c->done) && time_us_64() < dl) {
            sleep_ms(1);
        }
        if (!c->done) {
            printf("NET:   request timed out after %u ms\r\n", (unsigned)timeout_ms);
        } else if (c->status_code == 0) {
            printf("NET:   connection closed with no HTTP status (TLS/connect issue)\r\n");
        } else {
            printf("NET:   HTTP %d, %u bytes\r\n", c->status_code, (unsigned)c->resp_len);
        }
    }

    /* ALWAYS release the TLS pcb and its ~20 KB mbedTLS state back to the lwIP
     * heap. Without this, each beacon's GET leaks the buffers and the next
     * altcp_tls_new() in the same beacon fails with "out of memory". If gh_err()
     * already fired, lwIP freed the pcb and set c->pcb = NULL for us. */
    cyw43_arch_lwip_begin();
    if (c->pcb) {
        altcp_arg(c->pcb,  NULL);
        altcp_recv(c->pcb, NULL);
        altcp_sent(c->pcb, NULL);
        altcp_err(c->pcb,  NULL);
        if (altcp_close(c->pcb) != ERR_OK) {
            altcp_abort(c->pcb);   /* forced teardown if a graceful close fails */
        }
        c->pcb = NULL;
    }
    cyw43_arch_lwip_end();

    return c->done;
}

/* ---------------------------------------------------------------------------
 * Base64 encoder (RFC 4648, no line breaks)
 * --------------------------------------------------------------------------- */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *src, size_t src_len, char *dst) {
    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint32_t b = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) b |= (uint32_t)src[i+1] << 8;
        if (i + 2 < src_len) b |= (uint32_t)src[i+2];
        dst[out++] = b64_table[(b >> 18) & 0x3F];
        dst[out++] = b64_table[(b >> 12) & 0x3F];
        dst[out++] = (i + 1 < src_len) ? b64_table[(b >> 6) & 0x3F] : '=';
        dst[out++] = (i + 2 < src_len) ? b64_table[b & 0x3F]        : '=';
    }
    dst[out] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * GET current file SHA (needed by GitHub API for update vs create)
 * --------------------------------------------------------------------------- */
static bool gh_get_sha(char *sha_out, size_t sha_len,
                       struct altcp_tls_config *tls) {
    gh_client_t *c = &g_http;
    memset(c, 0, sizeof(*c));
    c->doing_get = true;
    c->req_len = (size_t)snprintf(c->req, sizeof(c->req),
        "GET /repos/%s/%s/contents/%s HTTP/1.1\r\n"
        "Host: api.github.com\r\n"
        "User-Agent: " FIRMWARE_VERSION "\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/vnd.github+json\r\n"
        "Connection: close\r\n\r\n",
        g_cfg.gh_user, g_cfg.gh_repo, g_cfg.gh_file, g_cfg.gh_token);

    gh_run(c, tls, 8000);
    if (!c->sha_ready) return false;
    snprintf(sha_out, sha_len, "%s", c->sha);
    return true;
}

/* ---------------------------------------------------------------------------
 * PUT JSON to GitHub
 * --------------------------------------------------------------------------- */
static bool gh_put(const char *json, size_t json_len,
                   const char *sha, struct altcp_tls_config *tls) {
    size_t b64_max = ((json_len + 2) / 3) * 4 + 4;
    char *b64 = (char *)malloc(b64_max);
    if (!b64) return false;
    base64_encode((const uint8_t *)json, json_len, b64);

    /* Commit message includes station and timestamp if available */
    char ts[32];
    net_fmt_utc(ts, sizeof(ts));

    size_t body_max = b64_max + 256;
    char *body = (char *)malloc(body_max);
    if (!body) { free(b64); return false; }

    size_t body_len;
    if (sha[0]) {
        body_len = (size_t)snprintf(body, body_max,
            "{\"message\":\"optical update from %s at %s\","
            "\"content\":\"%s\","
            "\"branch\":\"%s\","
            "\"sha\":\"%s\"}",
            g_cfg.device_id, ts, b64, g_cfg.gh_branch, sha);
    } else {
        body_len = (size_t)snprintf(body, body_max,
            "{\"message\":\"optical update from %s at %s\","
            "\"content\":\"%s\","
            "\"branch\":\"%s\"}",
            g_cfg.device_id, ts, b64, g_cfg.gh_branch);
    }
    free(b64);

    gh_client_t *c = &g_http;
    memset(c, 0, sizeof(*c));
    c->doing_get = false;
    c->req_len = (size_t)snprintf(c->req, sizeof(c->req),
        "PUT /repos/%s/%s/contents/%s HTTP/1.1\r\n"
        "Host: api.github.com\r\n"
        "User-Agent: " FIRMWARE_VERSION "\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/vnd.github+json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        g_cfg.gh_user, g_cfg.gh_repo, g_cfg.gh_file, g_cfg.gh_token,
        (unsigned)body_len, body);
    free(body);

    gh_run(c, tls, 15000);

    if (c->status_code == 401) { printf("GH_AUTH_FAIL\r\n"); return false; }
    if (c->status_code == 200 || c->status_code == 201) return true;
    printf("GH_PUT_FAIL status=%d\r\n", c->status_code);
    /* Surface GitHub's own error text (the JSON body after the header break) so a
     * 404/422 can be diagnosed: "Not Found" (wrong owner/repo or no token access),
     * "Bad credentials", "Resource not accessible by personal access token"
     * (missing Contents:write), "Invalid request" (bad branch/sha), etc. */
    const char *errbody = strstr(c->resp, "\r\n\r\n");
    if (errbody) printf("NET:   GitHub: %s\r\n", errbody + 4);
    return false;
}

/* ---------------------------------------------------------------------------
 * WiFi scan — list visible APs before attempting to connect (diagnostic aid)
 * --------------------------------------------------------------------------- */
static int net_scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r) return 0;
    char ssid[33];
    int n = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(ssid, r->ssid, (size_t)n);
    ssid[n] = '\0';
    printf("NET:   AP rssi=%4d chan=%2u auth=%u ssid=\"%s\"\r\n",
           (int)r->rssi, (unsigned)r->channel, (unsigned)r->auth_mode,
           ssid[0] ? ssid : "<hidden>");
    return 0;
}

static void net_scan_networks(void) {
    printf("NET: scanning for visible networks...\r\n");
    cyw43_wifi_scan_options_t opts = {0};
    cyw43_arch_lwip_begin();
    int err = cyw43_wifi_scan(&cyw43_state, &opts, NULL, net_scan_cb);
    cyw43_arch_lwip_end();
    if (err != 0) {
        printf("NET: scan failed to start (err=%d)\r\n", err);
        return;
    }
    /* Scan runs in the background context (core0); just wait for it to finish. */
    uint64_t dl = time_us_64() + 10000000ULL;   /* up to 10 s */
    for (;;) {
        cyw43_arch_lwip_begin();
        bool active = cyw43_wifi_scan_active(&cyw43_state);
        cyw43_arch_lwip_end();
        if (!active || time_us_64() > dl) break;
        sleep_ms(100);
    }
    printf("NET: scan complete\r\n");
}

/* ---------------------------------------------------------------------------
 * Connection helpers (shared by boot init and runtime reconnect)
 * --------------------------------------------------------------------------- */

/* Start SNTP once. lwIP SNTP poll mode then retries on its own schedule, and
 * sntp_set_time_callback() flips g_ntp_synced whenever a reply lands — so this
 * only needs to run after the FIRST successful (re)connect. */
static void ntp_start(void) {
    if (g_sntp_started) return;
    printf("NET: starting NTP sync (server=%s)...\r\n", g_cfg.ntp);
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, g_cfg.ntp);
    sntp_init();
    cyw43_arch_lwip_end();
    g_sntp_started = true;
}

/* Issue ONE async join and poll link status to UP (or a hard failure) within
 * timeout_ms. cyw43_arch_wifi_connect_async is an arch-level call that locks
 * internally (safe from any core in threadsafe_background); the lower-level
 * cyw43_tcpip_link_status read is wrapped per-call. The poll sleep is taken
 * WITHOUT the lock so the background context can service association/DHCP.
 * Returns true only on CYW43_LINK_UP. */
static bool wifi_join_once(uint32_t timeout_ms) {
    int err = cyw43_arch_wifi_connect_async(g_cfg.wifi_ssid, g_cfg.wifi_pass,
                                            CYW43_AUTH_WPA2_AES_PSK);
    if (err) { printf("NET:   connect_async err=%d\r\n", err); return false; }

    absolute_time_t until = make_timeout_time_ms(timeout_ms);
    int last = 0x7fffffff;
    while (!time_reached(until)) {
        cyw43_arch_lwip_begin();
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
        if (status != last) {
            printf("NET:   status=%d (%s)\r\n", status,
                   status == CYW43_LINK_JOIN   ? "JOIN" :
                   status == CYW43_LINK_NOIP   ? "NOIP" :
                   status == CYW43_LINK_UP     ? "UP"   :
                   status == CYW43_LINK_DOWN   ? "DOWN" :
                   status == CYW43_LINK_FAIL   ? "FAIL" :
                   status == CYW43_LINK_NONET  ? "NONET" :
                   status == CYW43_LINK_BADAUTH? "BADAUTH" : "?");
            last = status;
        }
        if (status == CYW43_LINK_UP) return true;
        if (status == CYW43_LINK_BADAUTH || status == CYW43_LINK_FAIL ||
            status == CYW43_LINK_NONET)
            return false;   /* transient/hard fail this pass — caller decides retry */
        led_wifi_blink();   /* all 3 LEDs blink 500/500 ms while connecting */
        sleep_ms(100);
    }
    return false;
}

/* Run up to max_attempts joins (leave + retry between). Updates g_wifi_connected
 * and the status LED. Re-runnable: used for the boot join (max_attempts=6) and
 * for a single runtime reconnect pass (max_attempts=1). See wifi_join_once for
 * the per-call locking rationale.
 *
 * Association is flaky on a moderate-RSSI guest AP: some attempts complete the
 * WPA2 handshake, others fail at AUTH/SET_SSID — so FAIL/BADAUTH/NONET are
 * "leave and retry", and only LINK_UP (IP leased) counts as success. */
static bool net_connect(int max_attempts) {
    g_wifi_connected = false;
    for (int attempt = 1; attempt <= max_attempts && !g_wifi_connected; attempt++) {
        printf("NET: join attempt %d/%d ...\r\n", attempt, max_attempts);
        if (wifi_join_once(20000)) { g_wifi_connected = true; break; }
        printf("NET:   attempt %d unsuccessful, leaving...\r\n", attempt);
        cyw43_arch_lwip_begin();
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
        sleep_ms(1000);
    }
    if (!g_wifi_connected) {
        printf("NET: WiFi connect FAILED after %d attempt(s)\r\n", max_attempts);
        led_wifi_failed();   /* red on, white/green off (no auto-reset — see note) */
        return false;
    }
    cyw43_arch_lwip_begin();
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    char ipstr[16];
    snprintf(ipstr, sizeof(ipstr), "%s", ip ? ip4addr_ntoa(ip) : "0.0.0.0");
    cyw43_arch_lwip_end();
    printf("NET: WiFi connected, ip=%s\r\n", ipstr);
    led_wifi_connected();   /* white on (steady) for normal operation */
    return true;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

bool net_init(void) {
    if (g_net_inited) return g_wifi_connected;

    /* cyw43_arch_init MUST run on the core that owns the GPIO (IO_IRQ_BANK0)
     * interrupts — core0, alongside COMP_OUT/button — because it installs the
     * chip's host-wake GPIO IRQ on the calling core and that chain is core-local.
     * net_init() is therefore called from main() on core0, before core1 starts
     * and before the COMP_OUT/button IRQs are registered, so cyw43 owns the GPIO
     * IRQ chain head.
     *
     * Use init_with_country so WIFI_COUNTRY is applied to the chip's regulatory
     * domain. A plain cyw43_arch_init() leaves it at the restrictive WORLDWIDE
     * default, which constrains association (a likely "scan works, join stalls"
     * cause, since scan is RX-only but joining transmits). */
    printf("NET: cyw43_arch_init (core=%d, country=0x%08x)...\r\n",
           (int)get_core_num(), (unsigned)WIFI_COUNTRY);
    if (cyw43_arch_init_with_country(WIFI_COUNTRY) != 0) {
        printf("NET: cyw43_arch_init failed\r\n");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    g_net_inited = true;

    net_scan_networks();

    printf("NET: connecting to WiFi (ssid=%s)...\r\n", g_cfg.wifi_ssid);
    /* Boot join: up to 6 attempts (the chip is slow to start associating, so a
     * full association + DHCP can take ~10-20 s per attempt — see net_connect /
     * wifi_join_once). In threadsafe_background mode the cyw43/lwIP/DHCP work is
     * serviced automatically by the low-priority IRQ; we only poll link status
     * and sleep. Do NOT use cyw43_arch_poll()/wait_for_work here — that combo
     * wedged core0 hard right as DHCP started. */
    if (!net_connect(6)) {
        /* Boot connect failed, but g_net_inited stays true (set above), so the
         * core1 loop's net_ensure_connected() keeps retrying with backoff — no
         * power-cycle needed once the AP returns. led_wifi_failed() already set. */
        return false;
    }

    ntp_start();
    uint64_t dl = time_us_64() + 10000000ULL;
    while (!g_ntp_synced && time_us_64() < dl) {
        sleep_ms(10);   /* background context services lwIP/SNTP on this core */
    }
    if (g_ntp_synced) printf("NET: NTP synced\r\n");
    else              printf("NET: NTP NOT synced yet — SNTP keeps polling\r\n");

    return g_ntp_synced;
}

/* Ensure the link is up before an upload, recovering from drops / a failed boot
 * connect. Cheap when already connected (a locked status read). When down, makes
 * a SINGLE join pass (net_connect(1)) no more often than the growing backoff
 * window allows, so we neither hammer the AP nor block core1 for long. Returns
 * true when associated with an IP. Safe to call every core1 loop. */
bool net_ensure_connected(void) {
    if (!g_net_inited) return false;   /* cyw43 bring-up never succeeded */

    cyw43_arch_lwip_begin();
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_lwip_end();

    if (st == CYW43_LINK_UP) {
        g_wifi_connected       = true;
        g_reconnect_backoff_ms = 0;
        if (!g_sntp_started) ntp_start();
        return true;
    }

    g_wifi_connected = false;
    if (!time_reached(g_next_reconnect)) return false;   /* still backing off */

    printf("NET: link down (status=%d), reconnecting...\r\n", st);
    if (net_connect(1)) {
        g_reconnect_backoff_ms = 0;
        if (!g_sntp_started) ntp_start();
        return true;
    }

    /* grow backoff 5s -> 10s -> 20s -> 40s -> 60s (cap) */
    g_reconnect_backoff_ms = g_reconnect_backoff_ms
        ? (g_reconnect_backoff_ms * 2u > 60000u ? 60000u : g_reconnect_backoff_ms * 2u)
        : 5000u;
    g_next_reconnect = make_timeout_time_ms(g_reconnect_backoff_ms);
    printf("NET: reconnect failed, next try in %u ms\r\n", (unsigned)g_reconnect_backoff_ms);
    return false;
}

bool net_wifi_connected(void) { return g_wifi_connected; }
bool net_ntp_synced(void)     { return g_ntp_synced; }

void net_fmt_utc(char *buf, size_t len) {
    if (!g_ntp_synced) { snprintf(buf, len, "null"); return; }
    /* g_ntp_epoch_us/g_ntp_boot_us are 64-bit and written in the SNTP callback
     * (cyw43 context, core0); snapshot them under the lwIP lock to avoid a torn
     * read on this core. */
    cyw43_arch_lwip_begin();
    uint64_t ntp_epoch_us = g_ntp_epoch_us, ntp_boot_us = g_ntp_boot_us;
    cyw43_arch_lwip_end();
    uint64_t epoch_us = ntp_epoch_us + (time_us_64() - ntp_boot_us);
    time_t t = (time_t)(epoch_us / 1000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

bool net_boot_to_epoch_us(uint64_t boot_us, uint64_t *epoch_us) {
    if (epoch_us) *epoch_us = 0;
    if (!g_ntp_synced) return false;
    /* Snapshot the 64-bit NTP fields under the lwIP lock — see net_fmt_utc(). */
    cyw43_arch_lwip_begin();
    uint64_t ntp_epoch_us = g_ntp_epoch_us, ntp_boot_us = g_ntp_boot_us;
    cyw43_arch_lwip_end();
    if (epoch_us) *epoch_us = ntp_epoch_us + (boot_us - ntp_boot_us);
    return true;
}

/* ---------------------------------------------------------------------------
 * Station status file — shared read-modify-write helpers.
 *
 * The detection event and the liveness beacon update the SAME flat JSON record,
 * each touching only its own fields and preserving the rest.
 * --------------------------------------------------------------------------- */

/* "YYYY-MM-DD HH:MM:SS" UTC for the status date fields. */
static void fmt_datetime(char *buf, size_t len) {
    if (!g_ntp_synced) { snprintf(buf, len, "1970-01-01 00:00:00"); return; }
    /* Snapshot the 64-bit NTP fields under the lwIP lock — see net_fmt_utc(). */
    cyw43_arch_lwip_begin();
    uint64_t ntp_epoch_us = g_ntp_epoch_us, ntp_boot_us = g_ntp_boot_us;
    cyw43_arch_lwip_end();
    uint64_t epoch_us = ntp_epoch_us + (time_us_64() - ntp_boot_us);
    time_t t = (time_t)(epoch_us / 1000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

/* Quoted "YYYY-MM-DD HH:MM:SS" UTC for a boot-relative time_us_64() instant, or
 * the bare JSON literal null if the clock has never synced. Converted at upload
 * time, so an event captured before the first NTP sync still gets its true time
 * once the clock is known. */
static void fmt_boot_instant_q(char *buf, size_t len, uint64_t boot_us) {
    uint64_t epoch_us = 0;
    if (!net_boot_to_epoch_us(boot_us, &epoch_us)) { snprintf(buf, len, "null"); return; }
    time_t t = (time_t)(epoch_us / 1000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "\"%Y-%m-%d %H:%M:%S\"", &tm);
}

/* Yield as "%.1f", or "null" when not derivable (negative). */
static void fmt_yield(char *buf, size_t len, float kt) {
    if (kt < 0.0f) snprintf(buf, len, "null");
    else           snprintf(buf, len, "%.1f", (double)kt);
}

/* Timing in ms as "%.1f", or "null" when not derivable (negative sentinel). */
static void fmt_ms(char *buf, size_t len, float ms) {
    if (ms < 0.0f) snprintf(buf, len, "null");
    else           snprintf(buf, len, "%.1f", (double)ms);
}

/* Locate the value after "key": in a flat JSON object. Returns a pointer to the
 * first character of the value and sets *val_end one past its last character,
 * or NULL if the key is absent. A leading '"' marks a string value (spanning to
 * its closing quote); otherwise the value runs to the next ',' or '}'. */
static char *json_value_span(char *json, const char *key, char **val_end) {
    char needle[48];
    int nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nn <= 0) return NULL;
    char *k = strstr(json, needle);
    if (!k) return NULL;
    char *p = k + nn;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *vstart = p;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') p++;
        if (*p == '"') p++;                 /* include the closing quote */
    } else {
        while (*p && *p != ',' && *p != '}') p++;
    }
    *val_end = p;
    return vstart;
}

/* Copy src to dst, setting one key's value: replace in place if the key exists,
 * otherwise insert it before the closing brace. new_value must include quotes
 * for a string field. Returns false only if the result would overflow dst. */
static bool json_upsert(const char *src, char *dst, size_t dst_sz,
                        const char *key, const char *new_value) {
    char *vend;
    char *vstart = json_value_span((char *)src, key, &vend);
    int w;
    if (vstart) {
        size_t prefix = (size_t)(vstart - src);
        w = snprintf(dst, dst_sz, "%.*s%s%s", (int)prefix, src, new_value, vend);
    } else {
        const char *brace = strrchr(src, '}');
        if (!brace) return false;
        size_t prefix = (size_t)(brace - src);
        w = snprintf(dst, dst_sz, "%.*s, \"%s\": %s%s",
                     (int)prefix, src, key, new_value, brace);
    }
    return w > 0 && (size_t)w < dst_sz;
}

/* Apply one upsert, ping-ponging between two equal-size work buffers so a chain
 * of updates needs only two buffers. On overflow the update is skipped. */
static void status_apply(char **src, char **dst, size_t sz,
                         const char *key, const char *val) {
    if (json_upsert(*src, *dst, sz, key, val)) {
        char *t = *src; *src = *dst; *dst = t;
    }
}

/* Build the canonical full status record (used when the file does not exist). */
static int status_default(char *buf, size_t sz, long total_minutes,
                          const char *date_q) {
    return snprintf(buf, sz,
        "{\"total minutes monitored\": %ld, "
        "\"station\": \"%s\", "
        "\"lat\": %s, "
        "\"long\": %s, "
        "\"last monitor upload date\": %s, "
        "\"nuke gone off?\": \"no\", "
        "\"nuke detected time\": null, "
        "\"yield_from_minima_kt\": null, "
        "\"yield_from_interval_kt\": null, "
        "\"t_peak_to_min_ms\": null, "
        "\"t_peak_to_peak_ms\": null, "
        "\"total_captures\": 0}",
        total_minutes, g_cfg.device_id, g_cfg.lat, g_cfg.lon, date_q);
}

/* Outcome of reading the current status file. Only STATUS_ABSENT licenses the
 * caller to create a fresh record; on STATUS_ERROR (timeout, 5xx, rate limit,
 * auth) the caller must give up and retry later, otherwise a transient failure
 * would overwrite the accumulated uptime and any recorded detection. */
typedef enum { STATUS_FOUND, STATUS_ABSENT, STATUS_ERROR } status_fetch_t;

/* Raw GET the current status file into out (NUL-terminated JSON object). */
static status_fetch_t status_fetch(struct altcp_tls_config *tls, char *out, size_t outsz) {
    gh_client_t *c = &g_http;
    memset(c, 0, sizeof(*c));
    c->doing_get = false;
    c->req_len = (size_t)snprintf(c->req, sizeof(c->req),
        "GET /repos/%s/%s/contents/%s HTTP/1.1\r\n"
        "Host: api.github.com\r\n"
        "User-Agent: " FIRMWARE_VERSION "\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/vnd.github.raw\r\n"
        "Connection: close\r\n\r\n",
        g_cfg.gh_user, g_cfg.gh_repo, g_cfg.gh_file, g_cfg.gh_token);
    bool complete = gh_run(c, tls, 8000);

    /* A 404 still returns a JSON error body, so the brace check alone is not
     * enough — require a 200 before treating the body as the real file. */
    if (c->status_code == 404) return STATUS_ABSENT;
    if (c->status_code != 200 || !complete) {
        printf("NET: status fetch failed (status=%d)\r\n", c->status_code);
        return STATUS_ERROR;
    }

    /* Search the body only, so a brace in a response header cannot match. */
    char *body   = strstr(c->resp, "\r\n\r\n");
    char *jstart = body ? strchr(body, '{') : NULL;
    char *jend   = jstart ? strrchr(jstart, '}') : NULL;
    /* A complete 200 with no JSON object is an empty/corrupt file: recreate it. */
    if (!jstart || !jend || jend <= jstart) return STATUS_ABSENT;
    *(jend + 1) = '\0';
    snprintf(out, outsz, "%s", jstart);
    return STATUS_FOUND;
}

/* Fetch the current SHA then PUT the given JSON content, retrying transient
 * failures. Each attempt re-GETs the SHA, so a stale-SHA 409/422 (a race with
 * the beacon, or a detection burst) self-heals on the next try. An auth/
 * permission error (401/403) is permanent — fail fast without retrying. */
static bool status_commit(struct altcp_tls_config *tls, const char *json) {
    static const uint32_t backoff_ms[] = { 0u, 2000u, 6000u };
    const size_t json_len = strlen(json);

    for (size_t attempt = 0; attempt < 3; attempt++) {
        if (backoff_ms[attempt]) sleep_ms(backoff_ms[attempt]);

        printf("NET: GET sha for %s/%s/%s ...\r\n",
               g_cfg.gh_user, g_cfg.gh_repo, g_cfg.gh_file);
        char sha[64] = "";
        bool have_sha = gh_get_sha(sha, sizeof(sha), tls);
        if (have_sha) printf("NET: sha=%s (update existing)\r\n", sha);
        else          printf("NET: no sha (file will be created)\r\n");

        printf("NET: PUT %u bytes (try %u/3) ...\r\n",
               (unsigned)json_len, (unsigned)(attempt + 1));
        if (gh_put(json, json_len, sha, tls)) return true;

        int sc = g_http.status_code;   /* PUT status set by the gh_put above */
        if (sc == 401 || sc == 403) {
            printf("NET: auth/permission error (status=%d), not retrying\r\n", sc);
            return false;
        }
        printf("NET: upload try %u failed (status=%d)\r\n",
               (unsigned)(attempt + 1), sc);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Detection event — read-modify-write the station status record.
 *
 * Always updates the two decoded yields (from time-to-minima and from the
 * inter-pulse interval) and the raw timings, and increments the lifetime
 * "total_captures" count held in the record itself. A REAL detection also sets
 * "nuke gone off?" = "yes" and "nuke detected time" (the time of the flash
 * onset, not of the upload). The uptime counter and all other fields are
 * preserved.
 * --------------------------------------------------------------------------- */
bool net_upload_optical(const char *source,
                        uint64_t t0_boot_us,
                        float t_min_ms,
                        float t_inter_ms,
                        float yield_tmin_kt,
                        float yield_tinter_kt) {
    if (!net_ensure_connected()) {
        printf("NET: not connected, skipping upload\r\n");
        return false;
    }

    cyw43_arch_lwip_begin();
    struct altcp_tls_config *tls = altcp_tls_create_config_client(NULL, 0);
    cyw43_arch_lwip_end();
    if (!tls) { printf("NET: TLS config alloc failed\r\n"); return false; }

    char ts[32];   fmt_datetime(ts, sizeof(ts));
    char ts_q[40]; snprintf(ts_q, sizeof(ts_q), "\"%s\"", ts);
    char det_q[40]; fmt_boot_instant_q(det_q, sizeof(det_q), t0_boot_us);
    char ym[16];   fmt_yield(ym, sizeof(ym), yield_tmin_kt);
    char yi[16];   fmt_yield(yi, sizeof(yi), yield_tinter_kt);
    char tm[16];   fmt_ms(tm, sizeof(tm), t_min_ms);
    char ti[16];   fmt_ms(ti, sizeof(ti), t_inter_ms);
    /* BIST no longer uploads, so this path is REAL-only; the alarm gate stays as
     * a safety belt so a stray non-REAL caller could never raise a false alarm. */
    bool real = (strcmp(source, "REAL") == 0);

    char b0[768], b1[768];
    char *src = b0, *dst = b1;
    long prev_captures = 0;
    switch (status_fetch(tls, src, sizeof(b0))) {
    case STATUS_FOUND: {
        char *vend, *v = json_value_span(src, "total_captures", &vend);
        if (v) prev_captures = strtol(v, NULL, 10);   /* null/garbage -> 0 */
        if (prev_captures < 0) prev_captures = 0;
        break;
    }
    case STATUS_ABSENT:
        status_default(src, sizeof(b0), 0, ts_q);
        break;
    default:
        /* Could not read the record: do not overwrite it. Caller retries. */
        cyw43_arch_lwip_begin();
        altcp_tls_free_config(tls);
        cyw43_arch_lwip_end();
        printf("NET: event upload FAILED (status record unreadable)\r\n");
        return false;
    }
    char nc[16];   snprintf(nc, sizeof(nc), "%ld", prev_captures + 1);

    /* Record the derived yields plus the raw timing observables that produced
     * them and the lifetime capture count. Only a REAL detection raises the
     * alarm (nuke gone off / detected time). */
    status_apply(&src, &dst, sizeof(b0), "yield_from_minima_kt",   ym);
    status_apply(&src, &dst, sizeof(b0), "yield_from_interval_kt", yi);
    status_apply(&src, &dst, sizeof(b0), "t_peak_to_min_ms",       tm);
    status_apply(&src, &dst, sizeof(b0), "t_peak_to_peak_ms",      ti);
    status_apply(&src, &dst, sizeof(b0), "total_captures",         nc);
    if (real) {
        status_apply(&src, &dst, sizeof(b0), "nuke gone off?",     "\"yes\"");
        status_apply(&src, &dst, sizeof(b0), "nuke detected time", det_q);
    }

    printf("NET: event %s yield_min=%s yield_int=%s t_min=%s t_inter=%s\r\n",
           source, ym, yi, tm, ti);
    printf("NET: content: %s\r\n", src);   /* status JSON has no trailing newline */
    bool ok = status_commit(tls, src);
    cyw43_arch_lwip_begin();
    altcp_tls_free_config(tls);
    cyw43_arch_lwip_end();

    printf("NET: event upload %s\r\n", ok ? "OK" : "FAILED");
    return ok;
}

/* ---------------------------------------------------------------------------
 * Liveness beacon — read-modify-write the same station status record.
 *
 * Adds BEACON_PERIOD_MIN to "total minutes monitored" (cumulative uptime) and refreshes
 * "last monitor upload date"; every other field (including detection values)
 * is preserved. Creates a fresh record if the file does not exist.
 * --------------------------------------------------------------------------- */
bool net_upload_beacon(void) {
    if (!net_ensure_connected()) {
        printf("NET: not connected, skipping beacon\r\n");
        return false;
    }

    cyw43_arch_lwip_begin();
    struct altcp_tls_config *tls = altcp_tls_create_config_client(NULL, 0);
    cyw43_arch_lwip_end();
    if (!tls) { printf("NET: TLS config alloc failed\r\n"); return false; }

    char ts[32];   fmt_datetime(ts, sizeof(ts));
    char ts_q[40]; snprintf(ts_q, sizeof(ts_q), "\"%s\"", ts);

    char b0[768], b1[768];
    char *src = b0, *dst = b1;

    status_fetch_t fetched = status_fetch(tls, src, sizeof(b0));
    if (fetched == STATUS_ERROR) {
        /* Could not read the record: skip this beacon rather than reset it. */
        cyw43_arch_lwip_begin();
        altcp_tls_free_config(tls);
        cyw43_arch_lwip_end();
        printf("NET: beacon upload FAILED (status record unreadable)\r\n");
        return false;
    }
    if (fetched == STATUS_FOUND) {
        long prev = 0;
        char *vend, *v = json_value_span(src, "total minutes monitored", &vend);
        if (v) prev = strtol(v, NULL, 10);
        char tot[16];
        snprintf(tot, sizeof(tot), "%ld", prev + (long)BEACON_PERIOD_MIN);
        status_apply(&src, &dst, sizeof(b0), "total minutes monitored",  tot);
        status_apply(&src, &dst, sizeof(b0), "last monitor upload date", ts_q);
        printf("NET: beacon total=%ld (prev=%ld)\r\n", prev + (long)BEACON_PERIOD_MIN, prev);
    } else {
        status_default(src, sizeof(b0), (long)BEACON_PERIOD_MIN, ts_q);
        printf("NET: beacon init (no existing record) total=%u\r\n", (unsigned)BEACON_PERIOD_MIN);
    }

    printf("NET: content: %s\r\n", src);   /* status JSON has no trailing newline */
    bool ok = status_commit(tls, src);
    cyw43_arch_lwip_begin();
    altcp_tls_free_config(tls);
    cyw43_arch_lwip_end();

    printf("NET: beacon upload %s\r\n", ok ? "OK" : "FAILED");
    return ok;
}
