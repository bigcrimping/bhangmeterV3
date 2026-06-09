#pragma once
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * BhangMeter V2 status LEDs — see bhangmeter_led_behaviour.md.
 *
 * Pins are free on pico2_w (no clash with SPI 16-19, ADC_CONVST 20, LATCH 8,
 * COMP_OUT 9, PUSH_BUTTON 12, BIST_EN 14).
 *   LED_W (white) = WiFi status
 *   LED_G (green) = BIST status
 *   LED_R (red)   = Error / NED status (latching)
 * --------------------------------------------------------------------------- */
#define LED_W_PIN  2
#define LED_G_PIN  6
#define LED_R_PIN  11

/* Set to 0 if the LEDs are wired active-low (GPIO sinks the cathode). */
#define LED_ACTIVE_HIGH 1

void led_init(void);            /* all off — startup state */
void led_wifi_blink(void);      /* call repeatedly in the connect loop: W+G blink 500/500 ms; R tracks NED latch */
void led_wifi_connected(void);  /* W on; G off; R tracks NED latch (does NOT clear it) */
void led_wifi_failed(void);     /* R on; W off; G off */
void led_ned_event(void);       /* R on, latched until reboot (BIST no longer clears it) */
void led_bist_start(void);      /* BIST begins: G steady on for the run */
void led_bist_pass(void);       /* G flashes 5x (1 s period, ~5 s); R restored to NED latch */
void led_bist_fail(void);       /* G steady + R flashes 5x (1 s period, ~5 s); both restored to NED latch */
void led_upload_error(void);    /* R on ~1 s then restore (non-latching; preserves NED latch) */
