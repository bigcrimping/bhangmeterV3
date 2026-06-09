/**
 * optical_bhang_runtime.c
 *
 * Bhangmeter BIST emitter, nuclear double-flash detection, waveform capture,
 * and serial command interface for the RP2350-based optical bhangmeter.
 *
 * This is the single firmware executable for the optical bhangmeter (see
 * CMakeLists.txt: target optical_bhang_runtime).
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/flash.h"
#include "pico/cyw43_arch.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "bhang_net.h"
#include "bhang_config.h"
#include "led.h"

/* ---------------------------------------------------------------------------
 * Pin and hardware constants
 * --------------------------------------------------------------------------- */
#define ADC_CONVST   20
#define SPI_SCK      18
#define SPI_CS       17
#define SPI_MISO     16
#define SPI_MOSI     19

#define BIST_EN      14
#define LATCH         8
#define COMP_OUT      9
#define PUSH_BUTTON  12
#define ADC_REF_MV   2780

/* ---------------------------------------------------------------------------
 * Waveform buffer
 * --------------------------------------------------------------------------- */
#define WAVE_BUF_SAMPLES    12288u   /* ~1.5 s at 7 kHz; caps max measurable inter-pulse time */
#define WAVE_SAMPLE_RATE_HZ 7000u
#define WAVE_INTERVAL_US    (1000000u / WAVE_SAMPLE_RATE_HZ)   /* 142 µs */
#define BASELINE_SAMPLES    32u

typedef struct {
    uint32_t t_us;     /* timestamp relative to t0 */
    uint16_t adc_raw;  /* 12-bit ADC value, right-justified */
} wave_sample_t;

static wave_sample_t wave_buf[WAVE_BUF_SAMPLES];
static uint32_t      wave_count = 0;

/* ---------------------------------------------------------------------------
 * Glasstone & Dolan low-altitude air-burst scaling
 *   t_inter (peak→peak)  ≈ 32.0 · W^0.44  ms
 *   t_min   (peak→min)   ≈  3.2 · W^0.44  ms
 * --------------------------------------------------------------------------- */
#define K_TIME_INTER_MS   32.0f
#define K_TIME_MIN_MS      3.2f
#define K_TIME_EXPONENT    0.44f
#define K_YIELD_EXPONENT  (1.0f / K_TIME_EXPONENT)   /* ≈ 2.273 */

static inline float bhang_t_inter_ms(float W_kt) {
    return K_TIME_INTER_MS * powf(W_kt, K_TIME_EXPONENT);
}
static inline float bhang_t_min_ms(float W_kt) {
    return K_TIME_MIN_MS * powf(W_kt, K_TIME_EXPONENT);
}
static inline float bhang_W_from_inter(float t_inter_ms) {
    return powf(t_inter_ms / K_TIME_INTER_MS, K_YIELD_EXPONENT);
}
static inline float bhang_W_from_min(float t_min_ms) {
    return powf(t_min_ms / K_TIME_MIN_MS, K_YIELD_EXPONENT);
}

/* Validity window: 10 kt – 1 Mt with 20% slack on each end */
#define T_INTER_MIN_MS    70.0f
#define T_INTER_MAX_MS   800.0f
#define T_MIN_MIN_MS       7.0f
#define T_MIN_MAX_MS      80.0f

/* ---------------------------------------------------------------------------
 * BIST yield presets
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *label;
    float       W_kt;
} bist_preset_t;

static const bist_preset_t PRESETS[] = {
    { "10kt",    10.0f },
    { "50kt",    50.0f },
    { "100kt",  100.0f },
    { "250kt",  250.0f },
    { "500kt",  500.0f },
    { "1mt",   1000.0f },
};
#define NUM_PRESETS 6

/* ---------------------------------------------------------------------------
 * State machine
 * --------------------------------------------------------------------------- */
typedef enum {
    STATE_IDLE      = 0,
    STATE_ARMED     = 1,
    STATE_CAPTURING = 2,
    STATE_ANALYSING = 3,
    STATE_DONE      = 4,
} bhang_state_t;

static const char *const STATE_NAMES[] = {
    "IDLE", "ARMED", "CAPTURING", "ANALYSING", "DONE"
};

static bhang_state_t g_state       = STATE_IDLE;
static uint8_t       g_preset_idx  = 2;   /* default: 100kt */

/* ---------------------------------------------------------------------------
 * Detection state — volatile members written in IRQ context
 * --------------------------------------------------------------------------- */
static volatile bool     g_capture_done    = false;
static volatile bool     g_start_capture   = false;  /* IRQ → main: begin ADC capture */
static volatile bool     g_edge_yield_done = false;  /* IRQ → main: 2nd edge seen, t_inter ready */
static volatile uint8_t  g_edge_phase      = 0;      /* 0=armed 1=post-fall1 2=post-rise1 */
static volatile bool     g_button_pending  = false;
static volatile uint32_t g_button_press_ms = 0;

/* ---------------------------------------------------------------------------
 * COMP_OUT edge log — timestamps of every threshold crossing, relative to t0
 * --------------------------------------------------------------------------- */
#define MAX_EDGE_EVENTS 8
typedef struct {
    uint32_t t_us;  /* microseconds since t0 */
    bool     onset; /* true = signal crossed threshold rising (flash onset)
                       false = signal crossed threshold falling (flash end)
                       Note: COMP_OUT is active-low, so onset = COMP_OUT falling edge */
} edge_event_t;

static edge_event_t g_edge_log[MAX_EDGE_EVENTS];
static uint8_t      g_edge_log_count = 0;

static uint64_t g_t0_epoch_us     = 0;
static float    g_yield_tmin_kt   = -1.0f;
static float    g_yield_tinter_kt = -1.0f;
static float    g_t_min_ms        = -1.0f;
static float    g_t_inter_ms      = -1.0f;
static uint32_t g_capture_count   = 0u;
static bool     g_yield_valid     = false;
static char     g_reject_reason[64];

/* ---------------------------------------------------------------------------
 * Serial command buffer
 * (long enough to hold "SET gh_token <fine-grained PAT>" — PATs run ~93 chars)
 * --------------------------------------------------------------------------- */
#define CMD_BUF_LEN 200
static char     cmd_buf[CMD_BUF_LEN];
static uint16_t cmd_pos = 0;

/* ---------------------------------------------------------------------------
 * Cross-core result handoff (core0 detection → core1 networking)
 * --------------------------------------------------------------------------- */
typedef struct {
    char     source[8];   /* "REAL" / "BIST" */
    float    t_min_ms;
    float    t_inter_ms;
    float    yield_tmin_kt;
    float    yield_tinter_kt;
    uint32_t capture_count;
} upload_msg_t;

#define RESULT_QUEUE_DEPTH 4
static queue_t g_result_q;

/* True while first-boot config mode is active (set in main, cleared by SAVE). */
static volatile bool g_config_mode = false;

/* ---------------------------------------------------------------------------
 * ADC wrapper — AD7091R over SPI
 * --------------------------------------------------------------------------- */
/**
 * Trigger one AD7091R conversion via CONVST pulse then read 16-bit SPI word.
 * Returns 12-bit right-justified result.  No FP, no heap.
 */
/* Datasheet Fig 28: CONVST falling edge starts conversion, must return HIGH
 * within tCONV (≤650 ns) to avoid power-down.  Two gpio_puts take ~20-50 ns. */
static inline void adc_start_conversion(void) {
    gpio_put(ADC_CONVST, 0);
    gpio_put(ADC_CONVST, 1);
}

/* Read SPI result — caller must ensure ≥650 ns elapsed since adc_start_conversion(). */
static inline uint16_t adc_read_result(void) {
    gpio_put(SPI_CS, 0);
    uint8_t tx[2] = {0x00, 0x00};
    uint8_t rx[2] = {0x00, 0x00};
    spi_write_read_blocking(spi0, tx, rx, 2);
    gpio_put(SPI_CS, 1);
    return (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]) >> 4;
}

static uint16_t adc_read_sample(void) {
    adc_start_conversion();
    sleep_us(1);
    return adc_read_result();
}

/* ---------------------------------------------------------------------------
 * State machine helper
 * --------------------------------------------------------------------------- */
static void set_state(bhang_state_t s) {
    g_state = s;
    char buf[32];
    snprintf(buf, sizeof(buf), "STATE %s\r\n", STATE_NAMES[s]);
    printf("%s", buf);
}

/* forward declaration — defined after waveform capture section */
static void run_capture(uint64_t t0_us);

/* ---------------------------------------------------------------------------
 * BIST double-flash emitter
 * --------------------------------------------------------------------------- */

/* Alarm callbacks toggle BIST_EN from hardware timer IRQ while run_capture()
 * busy-waits, so the ADC sees the real flash waveform. */
static int64_t bist_end_flash1(alarm_id_t id, void *user_data) {
    (void)id; (void)user_data;
    gpio_put(BIST_EN, 1);
    return 0;
}
static int64_t bist_start_flash2(alarm_id_t id, void *user_data) {
    (void)id; (void)user_data;
    gpio_put(BIST_EN, 0);
    return 0;
}
static int64_t bist_end_flash2(alarm_id_t id, void *user_data) {
    (void)id; (void)user_data;
    gpio_put(BIST_EN, 1);
    return 0;
}

/**
 * Emit a synthetic nuclear double-flash for preset[idx] and capture it into
 * wave_buf exactly as a real detection would.  Flash transitions are driven
 * by hardware timer alarms so run_capture() samples the ADC uninterrupted.
 */
static void bist_run(uint8_t idx) {
    float    t_inter = bhang_t_inter_ms(PRESETS[idx].W_kt);
    float    t_min   = bhang_t_min_ms(PRESETS[idx].W_kt);
    gpio_put(LATCH, 0);

    uint64_t t0 = time_us_64();
    uint64_t t1 = t0 + (uint64_t)(t_min * 1000.0f);                     /* flash1 end: T_min for preset */
    uint64_t t2 = t0 + (uint64_t)(t_inter * 1000.0f);                  /* flash2 start: T_inter after flash1 peak */
    uint64_t t3 = t2 + 80000u;                                           /* flash2 end: 80 ms */

    add_alarm_at(from_us_since_boot(t1), bist_end_flash1,   NULL, false);
    add_alarm_at(from_us_since_boot(t2), bist_start_flash2, NULL, false);
    add_alarm_at(from_us_since_boot(t3), bist_end_flash2,   NULL, false);

    gpio_put(BIST_EN, 0);
    g_t0_epoch_us = t0;
    run_capture(t0);
}

/* ---------------------------------------------------------------------------
 * Waveform capture
 * --------------------------------------------------------------------------- */
/**
 * Fill wave_buf at WAVE_SAMPLE_RATE_HZ starting at t0_us.
 * Uses a lightweight inline phase tracker to stop early once the second-pulse
 * decay has been detected.  No FP, no heap, no DMA.
 * LATCH is held low throughout so the comparator output can change freely.
 */
static void run_capture(uint64_t t0_us) {
    wave_count = 0;
    /* LATCH already low from arm_internal */

    /* causal 5-sample boxcar state */
    uint32_t swin[5]    = {0};
    uint32_t ssum       = 0;
    uint8_t  sptr       = 0;
    uint8_t  sfilled    = 0;
    int32_t  prev_sm    = 0;
    int32_t  prev_d     = 0;

    /* inline phase: 0=pre-peak1, 1=post-peak1, 2=post-min, 3=post-peak2 */
    uint8_t  phase      = 0;
    bool     done       = false;
    bool     first      = true;  /* first sample is taken immediately (no pacing wait) */

    uint64_t t_next     = t0_us;

    while (wave_count < WAVE_BUF_SAMPLES && !done) {
        uint64_t now;
        uint16_t raw;

        if (first) {
            /* Take the first sample right away — adc_read_sample() starts its own
             * conversion (CONVST pulse + settle), so there is nothing to wait for;
             * just anchor the pacing clock for subsequent samples. */
            now    = time_us_64();
            raw    = adc_read_sample();
            t_next = now + WAVE_INTERVAL_US;
            first  = false;
        } else {
            /* pace to sample rate */
            do { now = time_us_64(); } while (now < t_next);
            t_next += WAVE_INTERVAL_US;
            raw = adc_read_sample();
        }
        wave_buf[wave_count].t_us    = (uint32_t)(now - t0_us);
        wave_buf[wave_count].adc_raw = raw;
        wave_count++;

        /* update boxcar */
        ssum -= swin[sptr];
        swin[sptr] = raw;
        ssum += raw;
        sptr = (uint8_t)((sptr + 1u) % 5u);
        if (sfilled < 5) sfilled++;
        int32_t cur_sm = (int32_t)(ssum / sfilled);
        int32_t cur_d  = cur_sm - prev_sm;

        switch (phase) {
        case 0:  /* seeking first-pulse peak (d: + → ≤0) */
            if (prev_d > 0 && cur_d <= 0) phase = 1;
            break;
        case 1:  /* seeking inter-pulse minimum (d: − → ≥0) */
            if (prev_d < 0 && cur_d >= 0) phase = 2;
            break;
        case 2:  /* seeking second-pulse peak (d: + → ≤0) */
            if (prev_d > 0 && cur_d <= 0) phase = 3;
            break;
        case 3:  /* waiting for signal to decay to ≤5% of full scale */
            if (cur_sm <= (int32_t)(4095 * 5 / 100)) done = true;
            break;
        }

        prev_sm = cur_sm;
        prev_d  = cur_d;
    }
}

/* ---------------------------------------------------------------------------
 * Waveform analysis
 * --------------------------------------------------------------------------- */

/**
 * Apply a 5-sample centred boxcar to wave_buf[0..n-1].
 * Edge samples are clamped to the nearest valid value.
 * Output written to out[0..n-1] (caller-supplied, length n).
 */
static void smooth_buffer(uint32_t n, int32_t *out) {
    for (uint32_t i = 0; i < n; i++) {
        int64_t acc = 0;
        for (int d = -2; d <= 2; d++) {
            int32_t j = (int32_t)i + d;
            if (j < 0)          j = 0;
            if (j >= (int32_t)n) j = (int32_t)n - 1;
            acc += wave_buf[j].adc_raw;
        }
        out[i] = (int32_t)(acc / 5);
    }
}

/**
 * Analyse wave_buf using derivative sign-change detection on a smoothed copy.
 * Extracts first-pulse peak, inter-pulse minimum, and second-pulse peak
 * timestamps; computes yield via two independent estimators.
 *
 * Returns true on success.  On failure, sets g_reject_reason and returns false.
 */
static bool analyse_waveform(void) {
    if (wave_count < 64u) {
        snprintf(g_reject_reason, sizeof(g_reject_reason),
                 "INSUFFICIENT_SAMPLES %lu", (unsigned long)wave_count);
        return false;
    }

    /* static to avoid a 64 KB stack frame */
    static int32_t sm[WAVE_BUF_SAMPLES];
    smooth_buffer(wave_count, sm);

    /* baseline: minimum of smoothed inter-pulse region — computed after
     * we find the event markers, so first use simple BASELINE_SAMPLES mean */
    int64_t bsum = 0;
    uint32_t bn  = (wave_count < BASELINE_SAMPLES) ? wave_count : BASELINE_SAMPLES;
    for (uint32_t i = 0; i < bn; i++) bsum += sm[i];
    int32_t baseline = (int32_t)(bsum / (int64_t)bn);

    /* signal range for amplitude-gated marker detection */
    int32_t sig_min = sm[0], sig_max = sm[0];
    for (uint32_t i = 1; i < wave_count; i++) {
        if (sm[i] < sig_min) sig_min = sm[i];
        if (sm[i] > sig_max) sig_max = sm[i];
    }
    int32_t sig_mid = (sig_min + sig_max) / 2;

    /* derivative sign-change search */
    int32_t  fp_idx          = -1;   /* first-pulse peak index  */
    int32_t  im_idx          = -1;   /* inter-pulse minimum     */
    int32_t  sp_idx          = -1;   /* second-pulse peak index */
    uint32_t sp_search_start =  0;   /* skip noisy post-min shoulder before hunting second peak */

    for (uint32_t i = 3; i < wave_count; i++) {
        int32_t dp = sm[i-1] - sm[i-2];
        int32_t dc = sm[i]   - sm[i-1];

        if (fp_idx < 0) {
            if (dp > 0 && dc <= 0 && sm[i-1] > sig_mid) fp_idx = (int32_t)i - 1;
        } else if (im_idx < 0) {
            if (dp < 0 && dc >= 0 && sm[i-1] < sig_mid) {
                im_idx = (int32_t)i - 1;
                double t_min_meas = (wave_buf[im_idx].t_us - wave_buf[fp_idx].t_us) / 1000.0;
                sp_search_start = (uint32_t)im_idx + (uint32_t)(t_min_meas * (WAVE_SAMPLE_RATE_HZ / 1000.0));
            }
        } else if (sp_idx < 0 && i >= sp_search_start) {
            if (dp > 0 && dc <= 0 && sm[i-1] > sig_mid) sp_idx = (int32_t)i - 1;
        }
    }

    if (fp_idx < 0 || im_idx < 0 || sp_idx < 0) {
        snprintf(g_reject_reason, sizeof(g_reject_reason),
                 "EVENT_MARKERS_NOT_FOUND fp=%ld im=%ld sp=%ld",
                 (long)fp_idx, (long)im_idx, (long)sp_idx);
        return false;
    }

    /* refine baseline using the inter-pulse trough */
    baseline = sm[im_idx];
    for (int32_t i = fp_idx; i <= sp_idx; i++) {
        if (sm[i] < baseline) baseline = sm[i];
    }

    uint32_t t_fp_us = wave_buf[fp_idx].t_us;
    uint32_t t_im_us = wave_buf[im_idx].t_us;
    uint32_t t_sp_us = wave_buf[sp_idx].t_us;

    if (t_im_us <= t_fp_us || t_sp_us <= t_im_us) {
        snprintf(g_reject_reason, sizeof(g_reject_reason), "MARKER_ORDER_ERROR");
        return false;
    }

    double t_min_ms   = (t_im_us - t_fp_us) / 1000.0;
    double t_inter_ms = (t_sp_us - t_fp_us) / 1000.0;

    /* validity gate: T_inter must correspond to 10 kt – 1 Mt */
    bool tinter_ok = (t_inter_ms >= (double)T_INTER_MIN_MS && t_inter_ms <= (double)T_INTER_MAX_MS);
    bool tmin_ok   = (t_min_ms   >= (double)T_MIN_MIN_MS   && t_min_ms   <= (double)T_MIN_MAX_MS);
    if (!tinter_ok) {
        const char *ir = (t_inter_ms < (double)T_INTER_MIN_MS) ? "OUT_OF_RANGE_LOW" : "OUT_OF_RANGE_HIGH";
        const char *mr = tmin_ok ? "OK" : (t_min_ms < (double)T_MIN_MIN_MS ? "LOW" : "HIGH");
        snprintf(g_reject_reason, sizeof(g_reject_reason),
                 "T_INTER_%s %.0f ms T_MIN_%s %.0f ms",
                 ir, t_inter_ms, mr, t_min_ms);
        return false;
    }

    g_yield_tmin_kt   = bhang_W_from_min((float)t_min_ms);
    g_yield_tinter_kt = bhang_W_from_inter((float)t_inter_ms);
    g_t_min_ms        = (float)t_min_ms;
    g_t_inter_ms      = (float)t_inter_ms;
    g_yield_valid     = true;
    return true;
}

/* ---------------------------------------------------------------------------
 * Serial output helpers
 * --------------------------------------------------------------------------- */

/** Format a yield estimate as "XXX.X" or "N/A" into buf (caller supplies). */
static void format_kt(float val, char *buf, size_t len) {
    if (val < 0.0f) snprintf(buf, len, "N/A");
    else            snprintf(buf, len, "%.1f", (double)val);
}

/* ---------------------------------------------------------------------------
 * Hand a finished detection result to core1 for upload (non-blocking).
 * --------------------------------------------------------------------------- */
static void queue_upload(const char *source) {
    upload_msg_t m;
    snprintf(m.source, sizeof(m.source), "%s", source);
    m.t_min_ms        = g_t_min_ms;
    m.t_inter_ms      = g_t_inter_ms;
    m.yield_tmin_kt   = g_yield_tmin_kt;
    m.yield_tinter_kt = g_yield_tinter_kt;
    m.capture_count   = g_capture_count;
    if (!queue_try_add(&g_result_q, &m))
        printf("%s", "UPLOAD_DROPPED queue_full\r\n");
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * --------------------------------------------------------------------------- */

static void cmd_help(void) {
    printf("%s",
        "Commands:\r\n"
        "  BIST <10kt|50kt|100kt|250kt|500kt|1mt>  Run BIST double-flash\r\n"
        "  ARM                                Enable COMP_OUT detection\r\n"
        "  DISARM                             Disable interrupt, clear buffer\r\n"
        "  STATUS                             Show state and last yield estimates\r\n"
        "  DUMP                               Stream waveform buffer as CSV\r\n"
        "  SET <key> <value>                  Set a config field (see SHOW)\r\n"
        "  SHOW                               Print config (secrets masked)\r\n"
        "  SAVE                               Persist config to flash (IDLE only)\r\n"
        "  CLEAR                              Erase saved config (next boot)\r\n"
        "  HELP                               Print this list\r\n"
    );
}

/* ---------------------------------------------------------------------------
 * Config command handlers (SET / SHOW / SAVE / CLEAR)
 * --------------------------------------------------------------------------- */
static void cmd_set(char *arg) {
    /* core1 may read g_cfg (gh_user/gh_token/...) mid-upload; rewriting strings
     * here could hand it a torn value. Gate SET to IDLE like SAVE (normal flow is
     * DISARM -> SET ... -> SAVE -> ARM). First-boot config mode is not yet armed. */
    if (!g_config_mode && g_state != STATE_IDLE) {
        printf("%s", "ERR not_idle (DISARM first)\r\n");
        return;
    }
    char *key = arg;
    char *val = arg;
    while (*val && *val != ' ') val++;
    if (*val == ' ') { *val = '\0'; val++; }
    while (*val == ' ') val++;
    for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
    if (*key == '\0') { printf("%s", "ERR usage: SET <key> <value>\r\n"); return; }
    if (config_set(key, val)) printf("%s", "OK\r\n");
    else                      printf("ERR unknown key: %s\r\n", key);
}

static void cmd_show(void) {
    printf("%s", "CONFIG:\r\n");
    config_show();
}

static void cmd_save(void) {
    /* A flash erase stalls XIP and would corrupt a capture — only when idle.
     * During first-boot config mode the detector is not yet armed, so allow it. */
    if (!g_config_mode && g_state != STATE_IDLE) {
        printf("%s", "ERR not_idle (DISARM first)\r\n");
        return;
    }
    if (config_save()) {
        printf("%s", "OK SAVED\r\n");
        g_config_mode = false;   /* exit first-boot config loop if active */
    } else {
        printf("%s", "ERR save_failed\r\n");
    }
}

static void cmd_clear(void) {
    config_clear();
    printf("%s", "OK CLEARED (reboot to re-enter config mode)\r\n");
}

/* Internal arm — no state guard, safe to call after REJECT path. */
static void arm_internal(void) {
    g_capture_done    = false;
    g_start_capture   = false;
    g_edge_yield_done = false;
    g_edge_phase      = 0;
    g_edge_log_count  = 0;
    g_yield_valid     = false;
    gpio_put(LATCH, 0);
    gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL, true);
    set_state(STATE_ARMED);
}

static void cmd_arm(void) {
    if (g_state == STATE_CAPTURING || g_state == STATE_ANALYSING) {
        printf("%s", "ERR busy\r\n");
        return;
    }
    arm_internal();
}

static void cmd_disarm(void) {
    gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);
    g_capture_done    = false;
    g_start_capture   = false;
    g_edge_yield_done = false;
    g_edge_phase      = 0;
    g_edge_log_count  = 0;
    wave_count        = 0;
    set_state(STATE_IDLE);
}

static void cmd_status(void) {
    char kt1[16], kt2[16];
    format_kt(g_yield_tmin_kt,   kt1, sizeof(kt1));
    format_kt(g_yield_tinter_kt, kt2, sizeof(kt2));
    char buf[128];
    snprintf(buf, sizeof(buf),
             "state=%s preset=%s yield_tmin=%s kt yield_tinter=%s kt\r\n",
             STATE_NAMES[g_state], PRESETS[g_preset_idx].label, kt1, kt2);
    printf("%s", buf);
}

static void cmd_dump(void) {
    if (wave_count == 0u) {
        printf("%s", "ERR no_data\r\n");
        return;
    }
    char kt1[16], kt2[16], tm[16], ti[16], buf[80];
    format_kt(g_yield_tmin_kt,   kt1, sizeof(kt1));
    format_kt(g_yield_tinter_kt, kt2, sizeof(kt2));
    /* format_kt is a generic "%.1f or N/A" float formatter — reuse it for the
     * raw inter-pulse timing (negative sentinel → N/A on the edge-band path). */
    format_kt(g_t_min_ms,   tm, sizeof(tm));
    format_kt(g_t_inter_ms, ti, sizeof(ti));

    printf("%s", "DUMP_START\r\n");
    snprintf(buf, sizeof(buf), "T0_EPOCH_US: %llu\r\n",
             (unsigned long long)g_t0_epoch_us);
    printf("%s", buf);
    snprintf(buf, sizeof(buf), "SAMPLE_RATE_HZ: %u\r\n", WAVE_SAMPLE_RATE_HZ);
    printf("%s", buf);
    snprintf(buf, sizeof(buf), "SAMPLES: %lu\r\n", (unsigned long)wave_count);
    printf("%s", buf);
    snprintf(buf, sizeof(buf), "YIELD_TMIN_KT: %s\r\n", kt1);
    printf("%s", buf);
    snprintf(buf, sizeof(buf), "YIELD_TINTER_KT: %s\r\n", kt2);
    printf("%s", buf);
    snprintf(buf, sizeof(buf), "T_PEAK_TO_MIN_MS: %s\r\n", tm);
    printf("%s", buf);
    snprintf(buf, sizeof(buf), "T_PEAK_TO_PEAK_MS: %s\r\n", ti);
    printf("%s", buf);

    for (uint32_t i = 0; i < wave_count; i++) {
        uint32_t mv = (uint32_t)wave_buf[i].adc_raw * ADC_REF_MV / 4095u;
        snprintf(buf, sizeof(buf), "%lu,%lu\r\n",
                 (unsigned long)wave_buf[i].t_us,
                 (unsigned long)mv);
        printf("%s", buf);
    }

    printf("%s", "EDGES_START\r\n");
    for (uint8_t i = 0; i < g_edge_log_count; i++) {
        snprintf(buf, sizeof(buf), "%lu,%s\r\n",
                 (unsigned long)g_edge_log[i].t_us,
                 g_edge_log[i].onset ? "ONSET" : "END");
        printf("%s", buf);
    }
    printf("%s", "EDGES_END\r\n");

    printf("%s", "DUMP_END\r\n");
}

/**
 * Run a BIST double-flash for preset[idx], analyse, and report the result.
 *
 * BIST is a LOCAL self-test only: pass/fail is reported via the status LED
 * (green/red) and the serial BIST_DONE line. It never uploads, because its
 * yields are synthetic and would clobber the shared station status record's
 * real-detection fields. Remote liveness is the beacon's job; only a REAL
 * double-flash writes detection values to GitHub.
 *
 * Shared by the serial (cmd_bist) and button paths so both use ONE precondition.
 *
 * The detector's COMP_OUT edge IRQ is disabled for the duration of the run: the
 * BIST_EN flashes drive the same comparator the live detector watches, so leaving
 * it armed would fire comp_out_raw_handler concurrently with bist_run's own
 * run_capture (double-capture / state corruption). If the detector was armed on
 * entry it is re-armed afterward, so a serial/button BIST does not silently leave
 * the detector disarmed. Tradeoff: a real flash arriving during the BIST run
 * (≤~1.5 s) is missed — one comparator cannot BIST and detect at once.
 */
static void do_bist(uint8_t idx) {
    if (g_state == STATE_CAPTURING || g_state == STATE_ANALYSING) {
        printf("%s", "ERR busy\r\n");
        return;
    }
    bool was_armed = (g_state == STATE_ARMED);
    led_bist_start();   /* green steady on while the run proceeds */
    gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);

    set_state(STATE_CAPTURING);
    bist_run(idx);

    set_state(STATE_ANALYSING);
    char buf[96];
    if (analyse_waveform()) {
        char kt1[16], kt2[16];
        format_kt(g_yield_tmin_kt,   kt1, sizeof(kt1));
        format_kt(g_yield_tinter_kt, kt2, sizeof(kt2));
        snprintf(buf, sizeof(buf), "BIST_DONE %s YIELD tmin=%s kt tinter=%s kt\r\n",
                 PRESETS[idx].label, kt1, kt2);
        g_capture_count++;
        led_bist_pass();        /* green 2s; clears any latched NED on red */
        printf("%s", buf);
        /* Local self-test only — no upload (synthetic yields must not touch the
         * shared status record). Restore the prior detection state. */
        if (was_armed) arm_internal();
        else           set_state(STATE_DONE);
    } else {
        snprintf(buf, sizeof(buf), "BIST_DONE %s REJECT %s\r\n",
                 PRESETS[idx].label, g_reject_reason);
        led_bist_fail();
        printf("%s", buf);
        if (was_armed) arm_internal(); else set_state(STATE_IDLE);
    }
}

/**
 * Run BIST for a named preset (case-insensitive).
 * Also updates g_preset_idx so the button BIST uses the last-named preset.
 */
static void cmd_bist(const char *arg) {
    int found = -1;
    for (int i = 0; i < NUM_PRESETS; i++) {
        const char *lbl = PRESETS[i].label;
        const char *a   = arg;
        bool match = true;
        while (*lbl && *a) {
            if (tolower((unsigned char)*lbl) != tolower((unsigned char)*a)) {
                match = false; break;
            }
            lbl++; a++;
        }
        if (match && *lbl == '\0' && *a == '\0') { found = i; break; }
    }
    if (found < 0) {
        printf("%s",
            "ERR unknown preset; use: 10kt 50kt 100kt 250kt 500kt 1mt\r\n");
        return;
    }
    g_preset_idx = (uint8_t)found;
    do_bist((uint8_t)found);
}

/* ---------------------------------------------------------------------------
 * Command parser / dispatcher
 * --------------------------------------------------------------------------- */

/**
 * Process one complete NUL-terminated command line.
 * Echoes the line first, then dispatches case-insensitively.
 */
static void process_command(char *line) {
    /* skip leading spaces */
    while (*line == ' ') line++;

    /* uppercase verb in-place; split at first space */
    char *p = line;
    while (*p && *p != ' ') { *p = (char)toupper((unsigned char)*p); p++; }
    char *arg = p;
    if (*arg == ' ') { *arg = '\0'; arg++; }
    while (*arg == ' ') arg++;

    const char *verb = line;
    if      (strcmp(verb, "HELP")      == 0) cmd_help();
    else if (strcmp(verb, "ARM")       == 0) cmd_arm();
    else if (strcmp(verb, "DISARM")    == 0) cmd_disarm();
    else if (strcmp(verb, "STATUS")    == 0) cmd_status();
    else if (strcmp(verb, "DUMP")      == 0) cmd_dump();
    else if (strcmp(verb, "BIST")      == 0) cmd_bist(arg);
    else if (strcmp(verb, "SET")       == 0) cmd_set(arg);
    else if (strcmp(verb, "SHOW")      == 0) cmd_show();
    else if (strcmp(verb, "SAVE")      == 0) cmd_save();
    else if (strcmp(verb, "CLEAR")     == 0) cmd_clear();
    else printf("%s", "ERR unknown command\r\n");
}

/* ---------------------------------------------------------------------------
 * Serial input polling (non-blocking)
 * --------------------------------------------------------------------------- */
static void serial_poll(void) {
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            printf("\r\n");
            if (cmd_pos > 0) {
                cmd_buf[cmd_pos] = '\0';
                process_command(cmd_buf);
                cmd_pos = 0;
            }
        } else if ((c == 0x08 || c == 0x7F) && cmd_pos > 0u) {
            /* Backspace/DEL: drop the last char and erase it on the terminal so
             * long lines (e.g. SET gh_token ...) can be corrected. */
            cmd_pos--;
            printf("\b \b");
        } else if (cmd_pos < CMD_BUF_LEN - 1u && (unsigned char)c >= 0x20u && (unsigned char)c <= 0x7Eu) {
            cmd_buf[cmd_pos++] = (char)c;
            putchar(c);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Raw IRQ handler for COMP_OUT — highest order priority, runs before SDK dispatch.
 *
 * Three-phase edge state machine — records hardware timestamps only; yield is
 * computed from the two falling-edge times without any ADC peak detection.
 * ADC capture is delegated to the main loop for waveform dump purposes only.
 *
 * Phase 0 → 1 : first falling edge  (first flash exceeds threshold)  → t0
 * Phase 1 → 2 : rising edge         (first flash drops below threshold)
 * Phase 2 → 0 : second falling edge (second flash exceeds threshold)  → t1
 *
 * t_inter = t1 − t0.  Both crossings share the same threshold on the rising
 * slope, so the crossing-time bias cancels and t_inter ≈ peak-to-peak time.
 * --------------------------------------------------------------------------- */
static void comp_out_raw_handler(void) {
    uint64_t now    = time_us_64();
    uint32_t status = gpio_get_irq_event_mask(COMP_OUT);

    if (g_edge_phase == 0 && (status & GPIO_IRQ_EDGE_FALL)) {
        gpio_acknowledge_irq(COMP_OUT, GPIO_IRQ_EDGE_FALL);
        g_t0_epoch_us   = now;
        g_state         = STATE_CAPTURING;
        g_edge_phase    = 1;
        g_edge_log_count = 0;
        g_edge_log[0]   = (edge_event_t){ .t_us = 0u, .onset = true };
        g_edge_log_count = 1;
        g_start_capture = true;
        /* Arm exactly the next edge (RISE) and disable FALL, so comparator
         * chatter on the wrong edge can't double-advance the state machine. */
        gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL, false);
        gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_RISE, true);

    } else if (g_edge_phase == 1 && (status & GPIO_IRQ_EDGE_RISE)) {
        gpio_acknowledge_irq(COMP_OUT, GPIO_IRQ_EDGE_RISE);
        if (g_edge_log_count < MAX_EDGE_EVENTS) {
            g_edge_log[g_edge_log_count++] = (edge_event_t){
                .t_us = (uint32_t)(now - g_t0_epoch_us), .onset = false
            };
        }
        g_edge_phase = 2;
        /* Arm exactly the next edge (FALL) and disable RISE. */
        gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_RISE, false);
        gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL, true);

    } else if (g_edge_phase == 2 && (status & GPIO_IRQ_EDGE_FALL)) {
        gpio_acknowledge_irq(COMP_OUT, GPIO_IRQ_EDGE_FALL);
        gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);
        if (g_edge_log_count < MAX_EDGE_EVENTS) {
            g_edge_log[g_edge_log_count++] = (edge_event_t){
                .t_us = (uint32_t)(now - g_t0_epoch_us), .onset = true
            };
        }
        /* uint32 µs delta (same truncation as the edge log above) → single-
         * precision divide: a hardware VCVT + FP divide, no soft-double in the IRQ. */
        g_t_inter_ms          = (float)(uint32_t)(now - g_t0_epoch_us) / 1000.0f;   /* for main-loop band gate fallback */
        /* No libm in the IRQ: the yield (powf via bhang_W_from_inter) is computed
         * in the main loop — analyse_waveform() on the ADC path, or the edge-band
         * fallback when the ADC buffer was too short. */
        g_yield_valid         = true;
        g_edge_phase          = 0;
        g_edge_yield_done     = true;
    }
}

/* ---------------------------------------------------------------------------
 * PUSH_BUTTON raw handler. MUST be a raw handler (like COMP_OUT), NOT the
 * gpio_set_irq_enabled_with_callback() API: that callback API installs its own
 * shared IO_IRQ_BANK0 handler that conflicts with the cyw43 host-wake and
 * COMP_OUT raw handlers already on the chain, wedging core0 once WiFi is up.
 * --------------------------------------------------------------------------- */
static void push_button_raw_handler(void) {
    if (gpio_get_irq_event_mask(PUSH_BUTTON) & GPIO_IRQ_EDGE_FALL) {
        gpio_acknowledge_irq(PUSH_BUTTON, GPIO_IRQ_EDGE_FALL);
        g_button_press_ms = to_ms_since_boot(get_absolute_time());
        g_button_pending  = true;
    }
}

/* ---------------------------------------------------------------------------
 * Hardware initialisation
 * --------------------------------------------------------------------------- */
static void hw_init(void) {
    stdio_uart_init_full(uart0, 1000000, 0, 1);
    stdio_usb_init();
    sleep_ms(2000);   /* wait for USB-CDC to enumerate on host */
    printf("\r\nBOOT: stdio ok\r\n");

    spi_init(spi0, 36000000);
    spi_set_format(spi0, 8, 0, 0, SPI_MSB_FIRST);
    gpio_set_function(SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MISO, GPIO_FUNC_SPI);
    printf("BOOT: SPI ok\r\n");

    gpio_init(SPI_CS);     gpio_set_dir(SPI_CS,     GPIO_OUT); gpio_put(SPI_CS,     1);
    gpio_init(ADC_CONVST); gpio_set_dir(ADC_CONVST, GPIO_OUT); gpio_put(ADC_CONVST, 1);
    gpio_init(BIST_EN);    gpio_set_dir(BIST_EN,    GPIO_OUT); gpio_put(BIST_EN,    1);
    gpio_init(LATCH);      gpio_set_dir(LATCH,      GPIO_OUT); gpio_put(LATCH,      0);
    printf("BOOT: output GPIOs ok\r\n");

    led_init();   /* status LEDs (W/G/R) — all off = startup state */
    printf("BOOT: status LEDs ok\r\n");

    gpio_init(COMP_OUT);
    gpio_set_dir(COMP_OUT, GPIO_IN);

    gpio_init(PUSH_BUTTON);
    gpio_set_dir(PUSH_BUTTON, GPIO_IN);
    gpio_pull_up(PUSH_BUTTON);
    printf("BOOT: input GPIOs ok  COMP_OUT=%d PUSH_BUTTON=%d\r\n",
           gpio_get(COMP_OUT), gpio_get(PUSH_BUTTON));

    /* GPIO IRQ handlers (COMP_OUT, PUSH_BUTTON) are registered later, in
     * register_detection_irqs(), AFTER cyw43_arch_init. cyw43 installs its
     * host-wake handler on the IO_IRQ_BANK0 shared-handler chain on core0; that
     * chain must be created by cyw43 first, then our handlers spliced in — doing
     * it the other way round wedges the SDK's chain bookkeeping. */
    printf("BOOT: hw_init complete (GPIO IRQs deferred until after cyw43)\r\n");
}

/* Register the COMP_OUT + PUSH_BUTTON GPIO interrupt handlers on core0. MUST be
 * called after net_init()/cyw43_arch_init so cyw43 owns the IO_IRQ_BANK0 chain
 * head and our handlers are added afterwards. */
static void register_detection_irqs(void) {
    /* COMP_OUT: raw handler, highest order priority — enabled later by ARM.
     * The raw handler runs for any BANK0 event and filters on COMP_OUT itself. */
    gpio_add_raw_irq_handler_with_order_priority(COMP_OUT, comp_out_raw_handler,
                                                 PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
    gpio_set_irq_enabled(COMP_OUT, GPIO_IRQ_EDGE_FALL, false);
    printf("BOOT: COMP_OUT IRQ handler registered (disabled until ARM)\r\n");

    /* PUSH_BUTTON: raw handler, lower order priority than COMP_OUT. IO_IRQ_BANK0
     * is already enabled by cyw43 (cyw43_irq_init), so just add the handler and
     * enable this pin's edge event. */
    gpio_add_raw_irq_handler_with_order_priority(PUSH_BUTTON, push_button_raw_handler,
                                                 PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    gpio_set_irq_enabled(PUSH_BUTTON, GPIO_IRQ_EDGE_FALL, true);
    printf("BOOT: PUSH_BUTTON IRQ registered\r\n");
}

/* ---------------------------------------------------------------------------
 * core1 entry — networking. cyw43 is already up (initialised on core0, where its
 * host-wake GPIO IRQ must live alongside the COMP_OUT/button IRQs). This core
 * only drains the result queue and fires the liveness beacon, performing the
 * blocking HTTPS uploads off the detection core. All lwIP access inside
 * net_upload_* is wrapped in cyw43_arch_lwip_begin()/end() (threadsafe_background
 * mode), so it is safe to call from this core while cyw43 is serviced on core0.
 *
 * printf from this core is safe alongside core0's: pico_stdio guards every output
 * call with a shared mutex (both stdio_uart and stdio_usb are enabled), so there
 * is no character-level corruption — at worst two cores' whole lines interleave,
 * which is benign. Multi-field detection output (YIELD/REJECT) is pre-composed
 * into a single buffer and emitted with one printf to keep each line intact.
 * --------------------------------------------------------------------------- */
static void core1_main(void) {
    printf("NET: core1 networking started\r\n");

    /* Register core1 as a flash lockout victim so core0 can SAVE config safely,
     * and tell the config module that the multicore path is now live. */
    flash_safe_execute_core_init();
    config_set_dualcore(true);

    /* Drain detection results as they arrive, and fire a liveness beacon every
     * 5 minutes (the first one immediately after boot). */
    const uint32_t BEACON_PERIOD_MS = BEACON_PERIOD_MIN * 60u * 1000u;
    absolute_time_t next_beacon = get_absolute_time();
    upload_msg_t m;

    /* A REAL detection is critical data, so it is never dropped: if its upload
     * fails it is held here and retried on a growing backoff (capped at 5 min)
     * until it lands. BIST/beacon failures stay best-effort. The red NED LED
     * stays latched as the local indicator while a REAL upload is outstanding. */
    upload_msg_t    pending_real;
    bool            have_pending       = false;
    uint32_t        pending_backoff_ms = 0;
    absolute_time_t pending_next       = get_absolute_time();

    while (true) {
        /* Recover the link (or back off) before attempting any upload. */
        net_ensure_connected();

        if (queue_try_remove(&g_result_q, &m)) {
            printf("NET: dequeued %s event -> uploading\r\n", m.source);
            if (!net_upload_optical(m.source, m.t_min_ms, m.t_inter_ms,
                                    m.yield_tmin_kt, m.yield_tinter_kt, m.capture_count)) {
                led_upload_error();   /* transient red flash (preserves NED latch) */
                if (strcmp(m.source, "REAL") == 0 && !have_pending) {
                    pending_real       = m;
                    have_pending       = true;
                    pending_backoff_ms = 5000u;
                    pending_next       = make_timeout_time_ms(pending_backoff_ms);
                    printf("NET: REAL upload failed; holding for retry\r\n");
                } else if (strcmp(m.source, "REAL") == 0) {
                    printf("NET: REAL upload failed but a retry is already pending; dropped\r\n");
                }
            }
        }

        /* Retry a held REAL detection once its backoff elapses. */
        if (have_pending && time_reached(pending_next)) {
            printf("NET: retrying held REAL detection...\r\n");
            if (net_upload_optical(pending_real.source, pending_real.t_min_ms,
                                   pending_real.t_inter_ms, pending_real.yield_tmin_kt,
                                   pending_real.yield_tinter_kt, pending_real.capture_count)) {
                have_pending = false;
                printf("NET: held REAL detection uploaded\r\n");
            } else {
                led_upload_error();
                pending_backoff_ms = (pending_backoff_ms * 2u > 300000u)
                                   ? 300000u : pending_backoff_ms * 2u;
                pending_next = make_timeout_time_ms(pending_backoff_ms);
                printf("NET: REAL retry failed; next in %u ms\r\n", (unsigned)pending_backoff_ms);
            }
        }

        if (absolute_time_diff_us(get_absolute_time(), next_beacon) <= 0) {
            printf("NET: --- beacon tick ---\r\n");
            if (!net_upload_beacon())
                led_upload_error();
            next_beacon = make_timeout_time_ms(BEACON_PERIOD_MS);
        }
        sleep_ms(50);
    }
}

/* ---------------------------------------------------------------------------
 * First-boot config mode — loop on serial until the user issues SAVE.
 * Runs single-core (before core1 is launched), so config_save() uses the bare
 * flash path with no multicore lockout needed.
 * --------------------------------------------------------------------------- */
static void enter_config_mode(void) {
    printf("%s",
        "\r\n=== CONFIG MODE: no valid config in flash ===\r\n"
        "Set credentials, then SAVE. Example:\r\n"
        "  SET wifi_ssid MyNetwork\r\n"
        "  SET wifi_pass MyPassword\r\n"
        "  SET gh_token  github_pat_xxx\r\n"
        "  SHOW   (review)\r\n"
        "  SAVE   (persist + continue boot)\r\n");
    g_config_mode = true;
    while (g_config_mode) {
        serial_poll();
        tight_loop_contents();
    }
    printf("%s", "Config saved — continuing boot.\r\n");
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */
int main(void) {
    hw_init();
    printf("%s", "\r\nBHANGMETER RUNTIME v1.0\r\nType HELP for commands.\r\n");

    /* Load persisted config; on first boot (blank/invalid flash) seed defaults
     * and prompt over serial.  Must complete before core1 / networking start. */
    if (config_load()) {
        printf("CONFIG: loaded from flash (device_id=%s)\r\n", g_cfg.device_id);
    } else {
        config_seed_defaults();
        enter_config_mode();
    }

    /* Bring up cyw43 + WiFi/NTP on core0. cyw43_arch_init MUST run here: it
     * installs the chip's host-wake GPIO IRQ on the calling core, and GPIO
     * (IO_IRQ_BANK0) interrupts can only be managed from one core — the same core
     * that owns the COMP_OUT/button IRQs (core0). This blocks ~25 s at boot
     * (before detection is armed). After this, core1 performs the blocking
     * uploads using locked lwIP calls while cyw43 is serviced in the background
     * on core0. */
    net_init();

    /* Now that cyw43 owns the IO_IRQ_BANK0 chain, add our detection IRQs. */
    register_detection_irqs();

    /* Result queue + upload worker on core1. */
    queue_init(&g_result_q, sizeof(upload_msg_t), RESULT_QUEUE_DEPTH);
    multicore_launch_core1(core1_main);

    /* Start ARMED so the detector is live on boot (COMP_OUT IRQ enabled).
     * register_detection_irqs() above must have run first. Use DISARM over
     * serial to drop back to IDLE (e.g. before SAVE). */
    arm_internal();

    while (true) {
        serial_poll();

        /* debounced button press → BIST with current preset. do_bist() disarms
         * the detector for the run and self-guards on CAPTURING/ANALYSING, so the
         * button works whether the device is ARMED, IDLE, or DONE. */
        if (g_button_pending) {
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if ((now_ms - g_button_press_ms) >= 5u) {
                g_button_pending = false;
                if (!gpio_get(PUSH_BUTTON)) {   /* still low after debounce */
                    printf("%s", "BIST triggered by button\r\n");
                    do_bist(g_preset_idx);
                }
            }
        }

        /* first flash detected → run ADC capture (for dump) in main loop */
        if (g_start_capture) {
            g_start_capture = false;
            run_capture(g_t0_epoch_us);
            g_capture_done = true;
            /* if second flash never arrived during capture, reject and re-arm */
            if (!g_edge_yield_done) {
                printf("%s", "REJECT NO_SECOND_FLASH\r\n");
                set_state(STATE_IDLE);
                arm_internal();
            }
        }

        /* second flash edge received → qualify before declaring a NUDET.
         * A bare fall→rise→fall sequence is NOT sufficient: random light
         * double-blinks and comparator chatter produce that pattern too. Gate
         * the upload on analyse_waveform() (timing band + peak/min/peak shape),
         * falling back to a band check on the edge-measured t_inter only when
         * the ADC buffer was too short to analyse. */
        if (g_edge_yield_done) {
            g_edge_yield_done = false;

            float edge_t_inter_ms = g_t_inter_ms;  /* edge-measured, set in comp_out_raw_handler */
            g_t_min_ms      = -1.0f;
            g_yield_tmin_kt = -1.0f;

            set_state(STATE_ANALYSING);

            bool accept;
            if (g_capture_done && wave_count >= 64u) {
                accept = analyse_waveform();  /* sets g_reject_reason on fail; yield/timing globals on success */
            } else {
                accept = (edge_t_inter_ms >= T_INTER_MIN_MS && edge_t_inter_ms <= T_INTER_MAX_MS);
                if (accept)
                    /* powf moved out of the IRQ — derive the t_inter yield here */
                    g_yield_tinter_kt = bhang_W_from_inter(edge_t_inter_ms);
                else
                    snprintf(g_reject_reason, sizeof(g_reject_reason),
                             "T_INTER_OUT_OF_RANGE %.0f ms", (double)edge_t_inter_ms);
                g_t_inter_ms = edge_t_inter_ms;
            }

            if (accept) {
                led_ned_event();   /* real double-flash → latch red until next BIST pass */
                char kt1[16], kt2[16], buf[64];
                format_kt(g_yield_tmin_kt,   kt1, sizeof(kt1));
                format_kt(g_yield_tinter_kt, kt2, sizeof(kt2));
                snprintf(buf, sizeof(buf), "YIELD tmin=%s kt tinter=%s kt\r\n", kt1, kt2);
                printf("%s", buf);
                set_state(STATE_DONE);
                g_capture_count++;
                queue_upload("REAL");   /* core1 uploads; core0 re-arms immediately */
            } else {
                char buf[80];
                snprintf(buf, sizeof(buf), "REJECT %s\r\n", g_reject_reason);
                printf("%s", buf);
                set_state(STATE_IDLE);
            }

            /* Re-arm so a back-to-back event is caught while core1 is still
             * uploading the previous one (the queue absorbs the burst). */
            arm_internal();
        }
    }
}
