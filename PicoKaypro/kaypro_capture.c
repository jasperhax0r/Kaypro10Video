/**
 * kaypro_capture.c — Kaypro Character ROM Pixel Snooper
 *
 * Taps the 2732 character ROM (U26) data outputs instead of video RAM.
 * Each CCLK cycle the ROM outputs 8 pixel bits for one character column.
 * This gives true pixel data — no font rendering, no character encoding
 * issues, no CRTC memory offset problems.
 *
 * Wiring (SAME BSS138 board as before, just re-route data lines):
 *   GP0-GP7 = D0-D7  2732 character ROM pins 9-11, 13-17
 *   GP8     = DE      6545 CRTC pin 18 (Display Enable, active high)
 *   GP9     = CCLK    6545 CRTC pin 21 (~1.286 MHz)
 *   GP10    = VSYNC   6545 CRTC pin 16 (active low)
 *   GP11    = HSYNC   6545 CRTC pin 17
 *
 * Frame geometry:
 *   80 chars × 8 pixels = 640 pixels wide (1bpp, 80 bytes per scan line)
 *   240 active scan lines per frame (every line is unique — no skipping!)
 *   Frame = 80 × 240 = 19,200 bytes
 *
 * USB protocol (pull-based):
 *   Frame:  0xAA 0x55 0xAA 0x55 + 2B frame# + 2B length + 19200B
 *   Status: 0xBB 0x66 0xBB 0x66 + 1B length + JSON
 *   Stdin:  's'=send frame  'f'/'F'=vsync_offset+/-
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "video_sampler.pio.h"

// ── Pins ──────────────────────────────────────────────────────────────────────
#define PIN_DATA_BASE       0
#define PIN_HSYNC           8       // GP8 = HSYNC (active low) — line sync trigger
#define PIN_CCLK            9       // GP9 = CCLK ~1.286MHz
#define PIN_VSYNC           10      // GP10 = VSYNC (active low)
#define PIN_DE              11      // GP11 = Display Enable
#define PIN_UART_TX         12      // GP12 = UART TX → Pi Zero 2W

// ── Frame geometry ────────────────────────────────────────────────────────────
// Each CCLK = 8 pixel bits (one character column from ROM)
// 80 chars per active scan line = 80 bytes per line
// 240 active scan lines per frame
#define BYTES_PER_LINE      80      // full 80 chars per scan line = 640 pixels
#define LINES_PER_FRAME     400     // all active scan lines
#define FRAME_BYTES         (BYTES_PER_LINE * LINES_PER_FRAME)  // 19,200
#define FRAME_BYTES         (BYTES_PER_LINE * LINES_PER_FRAME)  // 19,200

// ── State ─────────────────────────────────────────────────────────────────────
static volatile int      vsync_to_row0 = 0;   // tuned with f/F keys

static uint8_t           framebuf[2][FRAME_BYTES];
static volatile int      capture_buf   = 0;
static volatile uint16_t frame_count   = 0;
static volatile uint32_t stat_fps      = 0;
static volatile uint32_t diag_timeouts = 0;

static PIO  pio_cap     = pio0;
static uint sm_char     = 0;
static uint offset_char = 0;
static int  dma_chan    = -1;
static dma_channel_config _dcfg;

static const uint8_t FRAME_MAGIC[4]  = {0xAA, 0x55, 0xAA, 0x55};
static const uint8_t STATUS_MAGIC[4] = {0xBB, 0x66, 0xBB, 0x66};


// ── USB ───────────────────────────────────────────────────────────────────────

static void usb_write(const uint8_t *buf, int len) {
    for (int i = 0; i < len; i++) putchar_raw(buf[i]);
}

static void send_frame_uart(int buf_idx) {
    uint8_t hdr[8];
    memcpy(hdr, FRAME_MAGIC, 4);
    hdr[4] = frame_count & 0xFF;
    hdr[5] = (frame_count >> 8) & 0xFF;
    hdr[6] = FRAME_BYTES & 0xFF;
    hdr[7] = (FRAME_BYTES >> 8) & 0xFF;
    uart_write_blocking(uart0, hdr, 8);
    uart_write_blocking(uart0, framebuf[buf_idx], FRAME_BYTES);
}

static void send_frame_usb(int buf_idx) {
    uint8_t hdr[8];
    memcpy(hdr, FRAME_MAGIC, 4);
    hdr[4] = frame_count & 0xFF;
    hdr[5] = (frame_count >> 8) & 0xFF;
    hdr[6] = FRAME_BYTES & 0xFF;
    hdr[7] = (FRAME_BYTES >> 8) & 0xFF;
    usb_write(hdr, 8);
    usb_write(framebuf[buf_idx], FRAME_BYTES);
}

static void send_status(void) {
    char json[256];
    int len = snprintf(json, sizeof(json),
        "{\"frame\":%u,\"fps\":\"%lu.%lu\","
        "\"bytes_per_line\":%d,\"lines\":%d,"
        "\"vsync_to_row0\":%d,\"timeouts\":%lu}",
        frame_count, stat_fps/10, stat_fps%10,
        BYTES_PER_LINE, LINES_PER_FRAME,
        vsync_to_row0, diag_timeouts);
    uint8_t hdr[5];
    memcpy(hdr, STATUS_MAGIC, 4);
    hdr[4] = len & 0xFF;
    usb_write(hdr, 5);
    usb_write((uint8_t*)json, len);
}


// ── Signal helpers ────────────────────────────────────────────────────────────

static inline void wait_vsync_falling(void) {
    absolute_time_t t = make_timeout_time_ms(100);
    while ( gpio_get(PIN_VSYNC) && !time_reached(t)) tight_loop_contents();
    while (!gpio_get(PIN_VSYNC) && !time_reached(t)) tight_loop_contents();
}

// Skip n scan lines — each iteration waits for one HIGH pulse then LOW
static void skip_lines(int n) {
    for (int i = 0; i < n; i++) {
        absolute_time_t t = make_timeout_time_ms(5);
        while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents(); // wait in LOW
        while ( gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents(); // wait HIGH pulse
        while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents(); // wait new LOW
    }
}


// ── DMA ───────────────────────────────────────────────────────────────────────

static inline void arm_dma(uint8_t *dest, int count) {
    dma_channel_configure(dma_chan, &_dcfg,
        dest, (uint8_t*)&pio_cap->rxf[sm_char] + 0,
        count, true);
}

static inline bool dma_wait_us(uint32_t us) {
    absolute_time_t t = make_timeout_time_us(us);
    while (dma_channel_is_busy(dma_chan)) {
        if (time_reached(t)) { dma_channel_abort(dma_chan); return false; }
    }
    return true;
}


// ── Capture loop ──────────────────────────────────────────────────────────────

static void capture_loop(void) {
    dma_chan = dma_claim_unused_channel(true);
    _dcfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&_dcfg, DMA_SIZE_8);
    channel_config_set_read_increment(&_dcfg, false);
    channel_config_set_write_increment(&_dcfg, true);
    channel_config_set_dreq(&_dcfg, pio_get_dreq(pio_cap, sm_char, false));

    absolute_time_t fps_timer = make_timeout_time_ms(1000);
    uint32_t fps_count    = 0;
    uint32_t timeouts     = 0;
    bool send_requested   = false;

    while (true) {
        // ── Commands ─────────────────────────────────────────────────────────
        while (true) {
            int c = getchar_timeout_us(0);
            if (c < 0) break;
            if      (c == 's') send_requested = true;
            else if (c == 'f') vsync_to_row0++;
            else if (c == 'F' && vsync_to_row0 > 0) vsync_to_row0--;
        }

        // ── Reset PIO BEFORE waiting for VSYNC ───────────────────────────────
        // This ensures PIO is clean and ready when VSYNC arrives,
        // minimizing the time between VSYNC and first capture
        pio_sm_set_enabled(pio_cap, sm_char, false);
        pio_sm_restart(pio_cap, sm_char);
        pio_sm_exec(pio_cap, sm_char, pio_encode_jmp(offset_char));
        pio_sm_clear_fifos(pio_cap, sm_char);
        pio_sm_set_enabled(pio_cap, sm_char, true);

        // ── Wait for VSYNC ────────────────────────────────────────────────────
        wait_vsync_falling();

        // Wait for stable HSYNC after VSYNC (3 complete line cycles)
        // HSYNC: HIGH=sync pulse(4us), LOW=active video(47us)
        { absolute_time_t t = make_timeout_time_ms(5);
          for (int i = 0; i < 3; i++) {
              while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents(); // in LOW
              while ( gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents(); // HIGH pulse
              while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents(); // new LOW
          }
        }

        // Skip vsync_to_row0 complete scan lines
        skip_lines(vsync_to_row0);

        capture_buf ^= 1;
        uint8_t *buf = framebuf[capture_buf];
        timeouts = 0;

        // ── Capture all 240 scan lines ────────────────────────────────────────
        // HSYNC timing on Kaypro 6545 (measured):
        // 6545 HSYNC (pin 39) timing from datasheet:
        //   HIGH = ~4us  = sync pulse (blanking, end of line)
        //   LOW  = ~47us = active video (80 CCLK characters)
        // Capture: wait for HIGH (sync pulse), then LOW (new line starts),
        // arm DMA immediately for 80 bytes = full character row.
        for (int line = 0; line < LINES_PER_FRAME; line++) {
            // 1. Wait for HSYNC HIGH (sync pulse)
            {
                absolute_time_t t = make_timeout_time_ms(2);
                while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents();
            }
            // 2. Wait for HSYNC LOW (end of sync pulse)
            {
                absolute_time_t t = make_timeout_time_ms(2);
                while ( gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents();
            }

            // --- We are now in the "Back Porch" (blanking). DE is definitely LOW. ---
            
            // 3. Force reset the PIO to ensure it isn't holding stale data from a previous glitch
            pio_sm_clear_fifos(pio_cap, sm_char);
            pio_sm_exec(pio_cap, sm_char, pio_encode_jmp(offset_char)); 

            // 4. Arm the DMA. It will sit patiently.
            arm_dma(buf + line * BYTES_PER_LINE, BYTES_PER_LINE);

            // 5. When DE goes HIGH, the PIO will rapidly fire 80 times, satisfying the DMA.
            //    Wait for it to finish.
            if (!dma_wait_us(300)) {
                timeouts++;
                memset(buf + line * BYTES_PER_LINE, 0x00, BYTES_PER_LINE);
                if (timeouts > 24) break;
            }
        }

        // ── Update diagnostics ────────────────────────────────────────────────
        diag_timeouts = timeouts;

        // ── USB ───────────────────────────────────────────────────────────────
        if (send_requested) { send_requested = false; send_frame_usb(capture_buf); }
        if (frame_count % 240 == 0) send_status();

        // ── UART → display Pi Zero 2W ────────────────────────────────────────
        // Noise filter: AND current frame with accumulated buffer.
        // A pixel bit must be set in BOTH this frame and the previous frame
        // to be transmitted — single-frame noise spikes are eliminated at source.
        // We send every 4th frame (blocking send = ~64ms at 3Mbaud).
        if (frame_count % 4 == 0) {
            // AND current frame into the send buffer (which holds previous frame)
            uint8_t *cur  = framebuf[capture_buf];
            uint8_t *send = framebuf[capture_buf ^ 1];  // other buffer = previous
            for (int i = 0; i < FRAME_BYTES; i++)
                send[i] = cur[i] & send[i];
            // Send the ANDed result
            // Temporarily point send_frame_uart at the ANDed buffer
            int saved_buf = capture_buf;
            capture_buf ^= 1;           // point to ANDed buffer
            send_frame_uart(capture_buf);
            capture_buf = saved_buf;    // restore
        }

        frame_count++;
        fps_count++;
        if (time_reached(fps_timer)) {
            stat_fps  = fps_count * 10;
            fps_count = 0;
            fps_timer = make_timeout_time_ms(1000);
        }
    }
}


// ── Startup diagnostics ───────────────────────────────────────────────────────

static void run_signals(void) {
    printf("=== Signal check (1s) ===\n");
    uint32_t hs_e=0, cclk_e=0, vs_e=0;
    bool lh=false, lc=false, lv=false;
    absolute_time_t end = make_timeout_time_ms(1000);
    while (!time_reached(end)) {
        bool h=gpio_get(PIN_HSYNC), c=gpio_get(PIN_CCLK), v=gpio_get(PIN_VSYNC);
        if (h&&!lh) hs_e++;
        if (c&&!lc) cclk_e++;
        if (v&&!lv) vs_e++;
        lh=h; lc=c; lv=v;
    }
    printf("HSYNC:%lu/s  CCLK:%lu/s  VSYNC:%lu/s\n", hs_e, cclk_e, vs_e);

    // VSYNC period
    wait_vsync_falling();
    absolute_time_t t0 = get_absolute_time();
    wait_vsync_falling();
    uint32_t vsync_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
    printf("VSYNC period: %luus\n", vsync_us);

    // HSYNC detailed timing - measure both HIGH and LOW periods
    {
        uint32_t high_us=0, low_us=0;
        for (int i = 0; i < 10; i++) {
            absolute_time_t t = make_timeout_time_ms(5);
            // Find HIGH
            while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents();
            absolute_time_t t0 = get_absolute_time();
            while ( gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents();
            high_us += (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
            // Measure LOW
            t0 = get_absolute_time();
            while (!gpio_get(PIN_HSYNC) && !time_reached(t)) tight_loop_contents();
            low_us += (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
        }
        printf("HSYNC HIGH(sync pulse): %luus  LOW(active video): %luus\n",
               high_us/10, low_us/10);
        printf("Active chars per line: ~%lu\n\n", (low_us/10)*1286/1000);
    }
}


// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(500);
    set_sys_clock_khz(200000, true);
    stdio_init_all();
    sleep_ms(1500);

    for (int i = 0; i < 12; i++) {
        gpio_init(i); gpio_set_dir(i, GPIO_IN); gpio_disable_pulls(i);
    }

    // UART to display Pi Zero 2W — GP12 TX at 3Mbaud
    // 3Mbaud = exact divisor with Pi Zero 2W default 48MHz UART clock
    uart_init(uart0, 3000000);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);

    printf("=== Kaypro Character ROM Pixel Snooper ===\n");
    printf("GP0-7:ROM data  GP8:HSYNC  GP9:CCLK  GP10:VSYNC\n");
    printf("Frame: %d bytes/line x %d lines = %d bytes\n",
           BYTES_PER_LINE, LINES_PER_FRAME, FRAME_BYTES);
    printf("s=snapshot  f/F=vsync_offset+/-\n\n");

    run_signals();

    // PIO: unconditional CCLK sampler (no DE gate)
    // DMA armed on DE rising edge, captures exactly 80 bytes
    offset_char = pio_add_program(pio_cap, &video_sampler_program);
    pio_sm_config cfg = video_sampler_program_get_default_config(offset_char);
    sm_config_set_in_pins(&cfg, PIN_DATA_BASE);
    sm_config_set_in_shift(&cfg, false, false, 8);  // left shift, no autopush
    sm_config_set_clkdiv(&cfg, 1.0f);

    // ADD THIS LINE: Tell the PIO that 'jmp pin' means check GP11
    sm_config_set_jmp_pin(&cfg, PIN_DE);

    pio_sm_init(pio_cap, sm_char, offset_char, &cfg);
    pio_sm_set_enabled(pio_cap, sm_char, true);

    printf("Starting pixel capture (vsync_offset=%d)...\n\n", vsync_to_row0);
    send_status();
    capture_loop();
    return 0;
}
