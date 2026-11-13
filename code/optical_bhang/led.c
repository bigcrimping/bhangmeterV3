#include "led.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* NED ("nuke detected") indication latches LED_R until reboot.
 * The latch is written only on core0 (detection sets it via led_ned_event(),
 * led_init() clears it; a BIST does not). core1 only *reads* it — the Wi-Fi blink/connect paths and
 * the upload-error flash restore R to g_ned_latched instead of clobbering it, so
 * a real detection stays lit through any Wi-Fi reconnect. A plain volatile bool
 * is sufficient: the single writer means single-word writes with no cross-core
 * read-modify-write. */
static volatile bool g_ned_latched = false;

static inline void led_set(uint pin, bool on) {
#if LED_ACTIVE_HIGH
    gpio_put(pin, on);
#else
    gpio_put(pin, !on);
#endif
}

void led_init(void) {
    const uint pins[3] = { LED_W_PIN, LED_G_PIN, LED_R_PIN };
    for (int i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
    }
    led_set(LED_W_PIN, false);
    led_set(LED_G_PIN, false);
    led_set(LED_R_PIN, false);
    g_ned_latched = false;
}

void led_wifi_blink(void) {
    /* Time-based so it is idempotent — safe to call at any rate from the connect
     * poll loop; phase flips every 500 ms. Blink W and G only; R tracks the NED
     * latch so a real detection stays lit through a reconnect. */
    bool on = ((to_ms_since_boot(get_absolute_time()) / 500u) & 1u) != 0u;
    led_set(LED_W_PIN, on);
    led_set(LED_G_PIN, on);
    led_set(LED_R_PIN, g_ned_latched);
}

void led_wifi_connected(void) {
    led_set(LED_W_PIN, true);
    led_set(LED_G_PIN, false);
    /* Preserve any latched NED — Wi-Fi connect must not clear a real detection. */
    led_set(LED_R_PIN, g_ned_latched);
}

void led_wifi_failed(void) {
    led_set(LED_R_PIN, true);
    led_set(LED_W_PIN, false);
    led_set(LED_G_PIN, false);
}

void led_ned_event(void) {
    g_ned_latched = true;
    led_set(LED_R_PIN, true);
}

/* ---------------------------------------------------------------------------
 * BIST result indication (non-blocking).
 *
 *   start : LED_G steady on for the duration of the run.
 *   pass  : LED_G flashes 5x — 500 ms on / 500 ms off (1 s period, ~5 s total),
 *           then LED_G off and LED_R restored to the NED latch.
 *   fail  : LED_G held steady while LED_R flashes 5x (same 1 s cadence, ~5 s),
 *           then LED_G off and LED_R restored to the NED latch.
 *
 * Driven by a self-rescheduling alarm on the default (core0) pool: do_bist()
 * runs on the core0 main loop, so blocking ~5 s here would extend the detector
 * blind spot and stall serial. The callback only toggles a GPIO, which is safe
 * from the alarm IRQ. A BIST no longer clears the NED latch — red is restored
 * to whatever it was before the run (g_ned_latched). */
#define BIST_ANIM_TICK_MS  500      /* half-period: 500 on + 500 off = 1 s */
#define BIST_ANIM_TICKS    10       /* 10 half-periods = 5 flashes */

static volatile struct {
    bool fail;   /* true: flash R while holding G steady; false: flash G */
    int  ticks;  /* half-periods remaining */
} g_bist_anim;
static volatile alarm_id_t g_bist_anim_alarm = -1;

static void bist_anim_stop(void) {
    if (g_bist_anim_alarm >= 0) { cancel_alarm(g_bist_anim_alarm); g_bist_anim_alarm = -1; }
}

static int64_t bist_anim_cb(alarm_id_t id, void *user) {
    (void)id; (void)user;
    uint flash_pin = g_bist_anim.fail ? LED_R_PIN : LED_G_PIN;
    if (--g_bist_anim.ticks <= 0) {
        /* done: green off, red back to the NED latch */
        led_set(LED_G_PIN, false);
        led_set(LED_R_PIN, g_ned_latched);
        g_bist_anim_alarm = -1;
        return 0;                                   /* one-shot: stop */
    }
    /* ticks counts 9..1; the LED began ON, so even => on, odd => off */
    led_set(flash_pin, (g_bist_anim.ticks % 2) == 0);
    return (int64_t)BIST_ANIM_TICK_MS * 1000;       /* reschedule +500 ms */
}

static void bist_anim_start(bool fail) {
    bist_anim_stop();                               /* supersede any in-flight sequence */
    g_bist_anim.fail  = fail;
    g_bist_anim.ticks = BIST_ANIM_TICKS;
    if (fail) led_set(LED_G_PIN, true);             /* hold green steady through the run */
    led_set(fail ? LED_R_PIN : LED_G_PIN, true);    /* first flash on */
    g_bist_anim_alarm = add_alarm_in_ms(BIST_ANIM_TICK_MS, bist_anim_cb, NULL, true);
}

void led_bist_start(void) {
    bist_anim_stop();           /* clear any leftover result animation */
    led_set(LED_G_PIN, true);
}

void led_bist_pass(void) {
    bist_anim_start(false);     /* green flashes 5x; NED latch preserved */
}

void led_bist_fail(void) {
    bist_anim_start(true);      /* green steady + red flashes 5x; NED latch preserved */
}

static int64_t led_upload_err_off_cb(alarm_id_t id, void *user) {
    (void)id; (void)user;
    /* Restore: keep R on if a real NED is latched, otherwise turn it off. */
    led_set(LED_R_PIN, g_ned_latched);
    return 0;   /* one-shot */
}

void led_upload_error(void) {
    led_set(LED_R_PIN, true);
    /* Non-blocking: this runs on core1 (upload worker), so use a one-shot alarm
     * to clear R after 1 s instead of sleep_ms() — blocking here stalls the
     * beacon and result-queue draining. The callback fires on the default
     * (core0) alarm pool; a gpio_put from there is fine. */
    add_alarm_in_ms(1000, led_upload_err_off_cb, NULL, true);
}
