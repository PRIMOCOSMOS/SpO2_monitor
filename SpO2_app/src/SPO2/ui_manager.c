#include "ui_manager.h"
#include "../third_party/lvgl/lvgl.h"
#include "../display_ctrl/display_ctrl.h"
#include "xil_cache.h"
#include <stdio.h>

/* Display geometry matching VDMA / main.c */
#define DISPLAY_WIDTH  800
#define DISPLAY_HEIGHT 480
#define WAVE_POINTS    480
#define WAVE_AREA_W    504
#define WAVE_AREA_H    156
#define RED_WAVE_X     18
#define RED_WAVE_Y     78
#define IR_WAVE_X      18
#define IR_WAVE_Y      283
#define UI_BUILD_TAG   "UI DBG v2026-06-04ONE"

/* Externals from main.c */
extern DisplayCtrl gDispCtrl;
extern uint8_t *pFrames[];

/* VDMA triple buffer cycling */
static uint32_t write_idx = 0;

/* LVGL v9 display handle */
static lv_display_t * disp;

/* Full-screen 32-bit render buffer for LVGL (XRGB8888) */
static uint32_t lv_buf[DISPLAY_WIDTH * DISPLAY_HEIGHT] __attribute__((aligned(64)));

static void fb_redraw_full_waves_one(uint8_t *fb);
static void fb_draw_metrics_one(uint8_t *fb);

/**
 * @brief LVGL v9 flush callback.
 *        LVGL renders into lv_buf (32-bit). We convert active area to
 *        24-bit RGB888 into the VDMA physical frame, flush D-Cache,
 *        then swap the displayed frame.
 */
static void disp_flush(lv_display_t * display, const lv_area_t * area, uint8_t * px_map)
{
    uint8_t * dst = pFrames[write_idx];
    uint32_t * src = (uint32_t *)px_map;

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = 0; y < h; y++) {
        int32_t dst_y = area->y1 + y;
        uint32_t * src_row = src + y * w;
        uint8_t  * dst_row = dst + dst_y * (DISPLAY_WIDTH * 3) + area->x1 * 3;

        for (int32_t x = 0; x < w; x++) {
            uint32_t pixel = src_row[x];        /* 0xAARRGGBB or 0xXXRRGGBB */
            dst_row[0] = (uint8_t)((pixel >> 16) & 0xFF);  /* Red   */
            dst_row[1] = (uint8_t)((pixel >> 8)  & 0xFF);  /* Green */
            dst_row[2] = (uint8_t)( pixel        & 0xFF);  /* Blue  */
            dst_row += 3;
        }
    }

    /* LVGL full-screen render can overwrite the direct framebuffer waveform.
     * Re-overlay the monitor traces immediately before cache flush/frame swap.
     */
    fb_redraw_full_waves_one(dst);
    fb_draw_metrics_one(dst);

    Xil_DCacheFlushRange((UINTPTR)dst, DISPLAY_HEIGHT * DISPLAY_WIDTH * 3);
    DisplayChangeFrame(&gDispCtrl, write_idx);
    write_idx = (write_idx + 1U) % DISPLAY_NUM_FRAMES;

    lv_display_flush_ready(display);
}

/* UI Objects */
static lv_obj_t * wave_red_area;
static lv_obj_t * wave_ir_area;
static lv_obj_t * line_red;
static lv_obj_t * line_ir;
static lv_point_t red_points[WAVE_POINTS];
static lv_point_t ir_points[WAVE_POINTS];
static uint32_t wave_wr_idx = 0;
static lv_obj_t * label_spo2;
static lv_obj_t * label_hr;
static lv_obj_t * label_status;
static lv_obj_t * label_red_raw;
static lv_obj_t * label_ir_raw;
static lv_obj_t * label_debug;
static lv_obj_t * led_status;
static lv_style_t style_panel;
static lv_style_t style_card;
static lv_style_t style_title;

/* Per-channel adaptive AC scaling for waveforms. */
typedef struct {
    float dc;
    float amp;
    float y_smooth;
    uint8_t initialized;
} wave_scale_t;

typedef struct {
    float dc;
    float lp;
    float amp;
    uint8_t initialized;
} sw_ppg_filter_t;

static wave_scale_t red_scale = {0};
static wave_scale_t ir_scale = {0};
static sw_ppg_filter_t red_sw_filter = {0};
static sw_ppg_filter_t ir_sw_filter = {0};
static uint8_t fb_wave_initialized = 0;
static int32_t last_red_y = WAVE_AREA_H / 2;
static int32_t last_ir_y = WAVE_AREA_H / 2;
static int g_fb_spo2 = -1;
static int g_fb_hr = -1;

static int32_t sw_ppg_display_filter(int32_t sample, sw_ppg_filter_t *f)
{
    float x = (float)sample;
    if (!f->initialized) {
        f->dc = x;
        f->lp = 0.0f;
        f->amp = 20.0f;
        f->initialized = 1U;
    }

    /* Software monitor filter, applied after the PL FIR.  This is not used to
     * replace the FIR; it is a display-conditioning stage like bedside monitors
     * use: residual baseline rejection, low-pass smoothing, and motion spike
     * limiting before AGC. */
    f->dc = 0.995f * f->dc + 0.005f * x;
    float ac = x - f->dc;

    float abs_lp = (f->lp >= 0.0f) ? f->lp : -f->lp;
    f->amp = 0.995f * f->amp + 0.005f * abs_lp;
    if (f->amp < 8.0f) f->amp = 8.0f;

    float max_step = 2.8f * f->amp + 20.0f;
    float delta = ac - f->lp;
    if (delta > max_step) ac = f->lp + max_step;
    if (delta < -max_step) ac = f->lp - max_step;

    /* 100 Hz input: this one-pole LPF keeps PPG morphology but removes chatter. */
    f->lp = 0.45f * f->lp + 0.55f * ac;
    return (int32_t)f->lp;
}

static int32_t scale_wave_ac(int32_t ac_sample, wave_scale_t *s)
{
    float ac = (float)ac_sample;
    float abs_ac = (ac >= 0.0f) ? ac : -ac;

    if (!s->initialized) {
        s->dc = ac;
        s->amp = (abs_ac > 4.0f) ? abs_ac : 4.0f;
        s->y_smooth = 50.0f;
        s->initialized = 1U;
    }

    /* Display conditioning for FIR-filtered PPG AC.  A residual baseline can
     * still appear because the FIR Compiler is packet-framed; remove only that
     * very slow residual from the already-filtered signal, then apply monitor
     * AGC.  The displayed trace remains strictly post-FIR.
     */
    s->dc = 0.990f * s->dc + 0.010f * ac;
    ac -= s->dc;
    abs_ac = (ac >= 0.0f) ? ac : -ac;

    if (abs_ac > s->amp) s->amp = 0.75f * s->amp + 0.25f * abs_ac;
    else                 s->amp = 0.9980f * s->amp + 0.0020f * abs_ac;
    if (s->amp < 1.0f) s->amp = 1.0f;

    float y = 50.0f + (ac * 46.0f) / s->amp;
    if (y < 8.0f) y = 8.0f;
    if (y > 92.0f) y = 92.0f;

    float target = 0.35f * s->y_smooth + 0.65f * y;
    float delta = target - s->y_smooth;
    if (delta > 18.0f) delta = 18.0f;
    if (delta < -18.0f) delta = -18.0f;
    s->y_smooth += delta;
    return (int32_t)(s->y_smooth + 0.5f);
}

static int32_t scale_wave(uint32_t raw, wave_scale_t *s)
{
    float x = (float)raw;
    if (s->dc <= 1.0f) s->dc = x;
    s->dc = 0.995f * s->dc + 0.005f * x;
    return scale_wave_ac((int32_t)(x - s->dc), s);
}

static void fb_put_pixel(uint8_t *fb, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) return;
    uint8_t *p = fb + ((y * DISPLAY_WIDTH + x) * 3);
    p[0] = r; p[1] = g; p[2] = b;
}

static void fb_fill_rect_one(uint8_t *fb, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int32_t yy = 0; yy < h; ++yy) {
        uint8_t *p = fb + (((y + yy) * DISPLAY_WIDTH + x) * 3);
        for (int32_t xx = 0; xx < w; ++xx) {
            p[0] = r; p[1] = g; p[2] = b; p += 3;
        }
    }
}

static void fb_flush_rect(uint8_t *fb, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (fb == NULL) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t yy = 0; yy < h; ++yy) {
        Xil_DCacheFlushRange((UINTPTR)(fb + (((y + yy) * DISPLAY_WIDTH + x) * 3)), (uint32_t)w * 3U);
    }
}

static void fb_draw_hseg(uint8_t *fb, int32_t x, int32_t y, int32_t len, int32_t th,
                         uint8_t r, uint8_t g, uint8_t b)
{
    fb_fill_rect_one(fb, x + th, y, len - 2 * th, th, r, g, b);
    fb_fill_rect_one(fb, x, y + 1, th, th - 2, r, g, b);
    fb_fill_rect_one(fb, x + len - th, y + 1, th, th - 2, r, g, b);
}

static void fb_draw_vseg(uint8_t *fb, int32_t x, int32_t y, int32_t len, int32_t th,
                         uint8_t r, uint8_t g, uint8_t b)
{
    fb_fill_rect_one(fb, x, y + th, th, len - 2 * th, r, g, b);
    fb_fill_rect_one(fb, x + 1, y, th - 2, th, r, g, b);
    fb_fill_rect_one(fb, x + 1, y + len - th, th - 2, th, r, g, b);
}

static void fb_draw_digit(uint8_t *fb, int d, int32_t x, int32_t y, int32_t scale,
                          uint8_t r, uint8_t g, uint8_t b)
{
    static const uint8_t segs[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    if (d < 0 || d > 9) return;
    int32_t w = 18 * scale;
    int32_t h = 32 * scale;
    int32_t th = 4 * scale;
    uint8_t s = segs[d];
    if (s & 0x01) fb_draw_hseg(fb, x, y, w, th, r, g, b);                 /* A */
    if (s & 0x02) fb_draw_vseg(fb, x + w - th, y, h / 2, th, r, g, b);    /* B */
    if (s & 0x04) fb_draw_vseg(fb, x + w - th, y + h / 2, h / 2, th, r, g, b); /* C */
    if (s & 0x08) fb_draw_hseg(fb, x, y + h - th, w, th, r, g, b);        /* D */
    if (s & 0x10) fb_draw_vseg(fb, x, y + h / 2, h / 2, th, r, g, b);     /* E */
    if (s & 0x20) fb_draw_vseg(fb, x, y, h / 2, th, r, g, b);             /* F */
    if (s & 0x40) fb_draw_hseg(fb, x, y + h / 2 - th / 2, w, th, r, g, b);/* G */
}

static void fb_draw_number(uint8_t *fb, int value, int digits, int32_t x, int32_t y, int32_t scale,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if (value < 0) return;
    int pow10 = 1;
    for (int i = 1; i < digits; ++i) pow10 *= 10;
    int32_t step = 23 * scale;
    int started = 0;
    for (int i = 0; i < digits; ++i) {
        int d = (value / pow10) % 10;
        if (d != 0 || i == digits - 1 || started) {
            fb_draw_digit(fb, d, x + i * step, y, scale, r, g, b);
            started = 1;
        }
        pow10 /= 10;
    }
}

static void fb_draw_metrics_one(uint8_t *fb)
{
    if (fb == NULL) return;
    /* Clear only the numeric regions inside right-side cards and redraw large
     * seven-segment-style numbers directly in the framebuffer.  This bypasses
     * LVGL label/printf/font issues completely.
     */
    fb_fill_rect_one(fb, 552, 90, 170, 72, 11, 22, 32);
    fb_fill_rect_one(fb, 552, 235, 170, 72, 11, 22, 32);

    if (g_fb_spo2 > 0) {
        uint8_t rr = (g_fb_spo2 < 90) ? 255 : 49;
        uint8_t gg = (g_fb_spo2 < 90) ? 59  : 255;
        uint8_t bb = (g_fb_spo2 < 90) ? 48  : 122;
        fb_draw_number(fb, g_fb_spo2, 3, 560, 96, 2, rr, gg, bb);
    }
    if (g_fb_hr > 0) {
        uint8_t rr = (g_fb_hr < 45 || g_fb_hr > 130) ? 255 : 255;
        uint8_t gg = (g_fb_hr < 45 || g_fb_hr > 130) ? 59  : 212;
        uint8_t bb = (g_fb_hr < 45 || g_fb_hr > 130) ? 48  : 59;
        fb_draw_number(fb, g_fb_hr, 3, 560, 241, 2, rr, gg, bb);
    }
}

static void fb_draw_metrics_all(void)
{
    for (uint32_t f = 0; f < DISPLAY_NUM_FRAMES; ++f) {
        uint8_t *fb = pFrames[f];
        if (!fb) continue;
        fb_draw_metrics_one(fb);
        fb_flush_rect(fb, 552, 90, 180, 220);
    }
}

static void fb_draw_line_one(uint8_t *fb, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             uint8_t r, uint8_t g, uint8_t b)
{
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        /* Thick trace with a small square brush; visually closer to bedside
         * monitor phosphor/LED traces and much easier to see for low-amplitude
         * PPG than a one-pixel line. */
        for (int32_t oy = -2; oy <= 2; ++oy) {
            for (int32_t ox = -1; ox <= 1; ++ox) {
                fb_put_pixel(fb, x0 + ox, y0 + oy, r, g, b);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fb_redraw_full_waves_one(uint8_t *fb)
{
    if (fb == NULL) return;
    fb_fill_rect_one(fb, RED_WAVE_X, RED_WAVE_Y, WAVE_AREA_W, WAVE_AREA_H, 3, 9, 14);
    fb_fill_rect_one(fb, IR_WAVE_X,  IR_WAVE_Y,  WAVE_AREA_W, WAVE_AREA_H, 3, 9, 14);
    for (uint32_t i = 1; i < WAVE_POINTS; ++i) {
        fb_draw_line_one(fb, RED_WAVE_X + red_points[i - 1].x, RED_WAVE_Y + red_points[i - 1].y,
                         RED_WAVE_X + red_points[i].x,     RED_WAVE_Y + red_points[i].y,
                         255, 45, 45);
        fb_draw_line_one(fb, IR_WAVE_X + ir_points[i - 1].x, IR_WAVE_Y + ir_points[i - 1].y,
                         IR_WAVE_X + ir_points[i].x,     IR_WAVE_Y + ir_points[i].y,
                         0, 229, 255);
    }
}

static void fb_draw_wave_sample(uint32_t idx, int32_t red_y, int32_t ir_y)
{
    int32_t x = (int32_t)((idx * (WAVE_AREA_W - 1U)) / (WAVE_POINTS - 1U));
    int32_t prev_idx = (idx == 0U) ? 0 : (int32_t)idx - 1;
    int32_t prev_x = (int32_t)(((uint32_t)prev_idx * (WAVE_AREA_W - 1U)) / (WAVE_POINTS - 1U));

    /* Draw only on the currently displayed frame for smooth real-time sweep.
     * Redrawing/flushing all three full wave areas on every point caused visible
     * stutter.  Full LVGL frame swaps still call fb_redraw_full_waves_one() to
     * re-overlay the complete trace on newly displayed frames.
     */
    uint32_t cur = gDispCtrl.curFrame;
    if (cur >= DISPLAY_NUM_FRAMES) cur = 0;
    uint8_t *fb = pFrames[cur];
    if (fb) {
        if (!fb_wave_initialized) {
            fb_fill_rect_one(fb, RED_WAVE_X, RED_WAVE_Y, WAVE_AREA_W, WAVE_AREA_H, 3, 9, 14);
            fb_fill_rect_one(fb, IR_WAVE_X,  IR_WAVE_Y,  WAVE_AREA_W, WAVE_AREA_H, 3, 9, 14);
        }

        int32_t erase_x = x + 3;
        if (erase_x >= WAVE_AREA_W) erase_x -= WAVE_AREA_W;
        fb_fill_rect_one(fb, RED_WAVE_X + erase_x, RED_WAVE_Y, 12, WAVE_AREA_H, 3, 9, 14);
        fb_fill_rect_one(fb, IR_WAVE_X  + erase_x, IR_WAVE_Y,  12, WAVE_AREA_H, 3, 9, 14);

        if (idx != 0U) {
            fb_draw_line_one(fb, RED_WAVE_X + prev_x, RED_WAVE_Y + last_red_y,
                             RED_WAVE_X + x,      RED_WAVE_Y + red_y, 255, 45, 45);
            fb_draw_line_one(fb, IR_WAVE_X + prev_x, IR_WAVE_Y + last_ir_y,
                             IR_WAVE_X + x,      IR_WAVE_Y + ir_y, 0, 229, 255);
        } else {
            fb_put_pixel(fb, RED_WAVE_X + x, RED_WAVE_Y + red_y, 255, 45, 45);
            fb_put_pixel(fb, IR_WAVE_X + x, IR_WAVE_Y + ir_y, 0, 229, 255);
        }

        int32_t dirty_x = x - 4;
        if (dirty_x < 0) dirty_x = 0;
        int32_t dirty_w = 18;
        if (dirty_x + dirty_w > WAVE_AREA_W) dirty_w = WAVE_AREA_W - dirty_x;
        fb_flush_rect(fb, RED_WAVE_X + dirty_x, RED_WAVE_Y, dirty_w, WAVE_AREA_H);
        fb_flush_rect(fb, IR_WAVE_X + dirty_x, IR_WAVE_Y, dirty_w, WAVE_AREA_H);
    }

    fb_wave_initialized = 1U;
    last_red_y = red_y;
    last_ir_y = ir_y;
}

static lv_obj_t *create_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                              const char *title, lv_color_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_add_style(panel, &style_panel, 0);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_add_style(label, &style_title, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 8, 4);

    return panel;
}

static lv_obj_t *create_metric_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                                    const char *caption, const char *unit, lv_color_t color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_card, 0);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x9AA7B2), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 8, 6);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_color(value, color, 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_48, 0);
    lv_obj_align(value, LV_ALIGN_LEFT_MID, 8, 10);

    lv_obj_t *unit_label = lv_label_create(card);
    lv_label_set_text(unit_label, unit);
    lv_obj_set_style_text_color(unit_label, lv_color_hex(0x9AA7B2), 0);
    lv_obj_set_style_text_font(unit_label, &lv_font_montserrat_24, 0);
    lv_obj_align(unit_label, LV_ALIGN_BOTTOM_RIGHT, -10, -12);

    return value;
}

void UI_Init(void) {
    /* 1. Create display and attach flush + full-screen buffer */
    disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_buffers(disp, lv_buf, NULL, sizeof(lv_buf), LV_DISPLAY_RENDER_MODE_FULL);

    /* 2. Styles */
    lv_style_init(&style_panel);
    lv_style_set_bg_color(&style_panel, lv_color_hex(0x071018));
    lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
    lv_style_set_border_color(&style_panel, lv_color_hex(0x24384A));
    lv_style_set_border_width(&style_panel, 1);
    lv_style_set_radius(&style_panel, 8);
    lv_style_set_pad_all(&style_panel, 0);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x0B1620));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, lv_color_hex(0x31475D));
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 10);
    lv_style_set_pad_all(&style_card, 0);

    lv_style_init(&style_title);
    lv_style_set_text_font(&style_title, &lv_font_montserrat_14);

    /* 3. Build medical-monitor screen */
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x02070B), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *top = lv_obj_create(screen);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, DISPLAY_WIDTH, 40);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0C1B28), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "SpO2 MONITOR  |  " UI_BUILD_TAG);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8F4FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 14, 0);

    led_status = lv_obj_create(top);
    lv_obj_set_size(led_status, 14, 14);
    lv_obj_align(led_status, LV_ALIGN_RIGHT_MID, -124, 0);
    lv_obj_set_style_radius(led_status, 7, 0);
    lv_obj_set_style_border_width(led_status, 0, 0);
    lv_obj_set_style_bg_opa(led_status, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(led_status, lv_color_hex(0xFF3B30), 0);

    lv_obj_t *status_caption = lv_label_create(top);
    lv_label_set_text(status_caption, "SENSOR");
    lv_obj_set_style_text_color(status_caption, lv_color_hex(0x9AA7B2), 0);
    lv_obj_align(status_caption, LV_ALIGN_RIGHT_MID, -58, 0);

    lv_obj_t *red_panel = create_panel(screen, 10, 50, 520, 195, "RED LED waveform", lv_color_hex(0xFF4040));
    wave_red_area = lv_obj_create(red_panel);
    lv_obj_set_pos(wave_red_area, 8, 28);
    lv_obj_set_size(wave_red_area, WAVE_AREA_W, WAVE_AREA_H);
    lv_obj_set_style_bg_color(wave_red_area, lv_color_hex(0x03090E), 0);
    lv_obj_set_style_bg_opa(wave_red_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wave_red_area, 0, 0);
    lv_obj_set_style_radius(wave_red_area, 0, 0);
    lv_obj_set_style_pad_all(wave_red_area, 0, 0);
    lv_obj_clear_flag(wave_red_area, LV_OBJ_FLAG_SCROLLABLE);

    line_red = lv_line_create(wave_red_area);
    lv_obj_set_style_line_color(line_red, lv_color_hex(0xFF2D2D), 0);
    lv_obj_set_style_line_width(line_red, 2, 0);
    lv_obj_set_style_line_rounded(line_red, false, 0);

    lv_obj_t *ir_panel = create_panel(screen, 10, 255, 520, 195, "IR waveform", lv_color_hex(0x00E5FF));
    wave_ir_area = lv_obj_create(ir_panel);
    lv_obj_set_pos(wave_ir_area, 8, 28);
    lv_obj_set_size(wave_ir_area, WAVE_AREA_W, WAVE_AREA_H);
    lv_obj_set_style_bg_color(wave_ir_area, lv_color_hex(0x03090E), 0);
    lv_obj_set_style_bg_opa(wave_ir_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wave_ir_area, 0, 0);
    lv_obj_set_style_radius(wave_ir_area, 0, 0);
    lv_obj_set_style_pad_all(wave_ir_area, 0, 0);
    lv_obj_clear_flag(wave_ir_area, LV_OBJ_FLAG_SCROLLABLE);

    line_ir = lv_line_create(wave_ir_area);
    lv_obj_set_style_line_color(line_ir, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_line_width(line_ir, 2, 0);
    lv_obj_set_style_line_rounded(line_ir, false, 0);

    for (uint32_t i = 0; i < WAVE_POINTS; ++i) {
        int32_t x = (int32_t)((i * (WAVE_AREA_W - 1U)) / (WAVE_POINTS - 1U));
        red_points[i].x = x;
        red_points[i].y = WAVE_AREA_H / 2;
        ir_points[i].x = x;
        ir_points[i].y = WAVE_AREA_H / 2;
    }
    lv_line_set_points(line_red, red_points, WAVE_POINTS);
    lv_line_set_points(line_ir, ir_points, WAVE_POINTS);

    label_spo2 = create_metric_card(screen, 545, 55, 240, 130, "SpO2", "%", lv_color_hex(0x31FF7A));
    label_hr   = create_metric_card(screen, 545, 200, 240, 130, "HEART RATE", "bpm", lv_color_hex(0xFFD43B));

    lv_obj_t *raw_card = lv_obj_create(screen);
    lv_obj_set_pos(raw_card, 545, 345);
    lv_obj_set_size(raw_card, 240, 105);
    lv_obj_add_style(raw_card, &style_card, 0);

    lv_obj_t *raw_title = lv_label_create(raw_card);
    lv_label_set_text(raw_title, "RAW ADC");
    lv_obj_set_style_text_color(raw_title, lv_color_hex(0x9AA7B2), 0);
    lv_obj_align(raw_title, LV_ALIGN_TOP_LEFT, 8, 7);

    label_red_raw = lv_label_create(raw_card);
    lv_label_set_text(label_red_raw, "RED: ------");
    lv_obj_set_style_text_color(label_red_raw, lv_color_hex(0xFF7070), 0);
    lv_obj_set_style_text_font(label_red_raw, &lv_font_montserrat_24, 0);
    lv_obj_align(label_red_raw, LV_ALIGN_LEFT_MID, 8, -8);

    label_ir_raw = lv_label_create(raw_card);
    lv_label_set_text(label_ir_raw, "IR : ------");
    lv_obj_set_style_text_color(label_ir_raw, lv_color_hex(0x60EFFF), 0);
    lv_obj_set_style_text_font(label_ir_raw, &lv_font_montserrat_24, 0);
    lv_obj_align(label_ir_raw, LV_ALIGN_LEFT_MID, 8, 24);

    label_debug = lv_label_create(raw_card);
    lv_label_set_text(label_debug, "S0 P0 I00 F0 ID?? M?? C??\nW? R? T00 U0 C0 INIT");
    lv_obj_set_width(label_debug, 224);
    lv_label_set_long_mode(label_debug, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label_debug, lv_color_hex(0xC8D4E0), 0);
    lv_obj_set_style_text_font(label_debug, &lv_font_montserrat_14, 0);
    lv_obj_align(label_debug, LV_ALIGN_BOTTOM_LEFT, 8, -2);

    /* Status overlay — hidden in normal operation */
    label_status = lv_label_create(screen);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF3B30), 0);
    lv_label_set_text(label_status, "SENSOR OFFLINE");
    lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void UI_UpdateMetrics(float spo2, int hr) {
    if (label_spo2 == NULL || label_hr == NULL) return;

    if (spo2 > 0.1f) {
        int spo2_i = (int)(spo2 + 0.5f);
        g_fb_spo2 = spo2_i;
        lv_label_set_text_fmt(label_spo2, "%d", spo2_i);
        lv_obj_set_style_text_color(label_spo2,
                                    (spo2_i < 90) ? lv_color_hex(0xFF3B30) : lv_color_hex(0x31FF7A), 0);
    } else {
        g_fb_spo2 = -1;
        lv_label_set_text(label_spo2, "--");
    }
    lv_obj_invalidate(label_spo2);

    if (hr > 0) {
        g_fb_hr = hr;
        lv_label_set_text_fmt(label_hr, "%d", hr);
        lv_obj_set_style_text_color(label_hr,
                                    (hr < 45 || hr > 130) ? lv_color_hex(0xFF3B30) : lv_color_hex(0xFFD43B), 0);
    } else {
        g_fb_hr = -1;
        lv_label_set_text(label_hr, "--");
    }
    lv_obj_invalidate(label_hr);

    /* Authoritative numeric display path: draw directly to the framebuffers so
     * values are visible even if LVGL text rendering/float formatting stalls.
     */
    fb_draw_metrics_all();
}

void UI_PushFilteredWaveform(int32_t red_ac, int32_t ir_ac) {
    if (line_red == NULL || line_ir == NULL) return;

    int32_t red_disp = sw_ppg_display_filter(red_ac, &red_sw_filter);
    int32_t ir_disp  = sw_ppg_display_filter(ir_ac,  &ir_sw_filter);
    int32_t yr = scale_wave_ac(red_disp, &red_scale);
    int32_t yi = scale_wave_ac(ir_disp,  &ir_scale);
    red_points[wave_wr_idx].y = WAVE_AREA_H - 1 - (yr * (WAVE_AREA_H - 1) / 100);
    ir_points[wave_wr_idx].y  = WAVE_AREA_H - 1 - (yi * (WAVE_AREA_H - 1) / 100);

    fb_draw_wave_sample(wave_wr_idx, red_points[wave_wr_idx].y, ir_points[wave_wr_idx].y);

    if (wave_red_area != NULL) lv_obj_invalidate(wave_red_area);
    if (wave_ir_area  != NULL) lv_obj_invalidate(wave_ir_area);
    lv_line_set_points(line_red, red_points, WAVE_POINTS);
    lv_line_set_points(line_ir,  ir_points,  WAVE_POINTS);
    if (wave_red_area != NULL) lv_obj_invalidate(wave_red_area);
    if (wave_ir_area  != NULL) lv_obj_invalidate(wave_ir_area);

    wave_wr_idx++;
    if (wave_wr_idx >= WAVE_POINTS) wave_wr_idx = 0;
}

void UI_PushWaveform(uint32_t red, uint32_t ir) {
    /* Compatibility wrapper only; production display should call
     * UI_PushFilteredWaveform() with PL FIR output.
     */
    UI_PushFilteredWaveform((int32_t)red - 131072, (int32_t)ir - 131072);
    if (label_red_raw != NULL) lv_label_set_text_fmt(label_red_raw, "RED: %lu", (unsigned long)red);
    if (label_ir_raw  != NULL) lv_label_set_text_fmt(label_ir_raw,  "IR : %lu", (unsigned long)ir);
}

void UI_UpdateSensorStatus(bool online) {
    if (label_status == NULL) return;

    if (!online) {
        lv_label_set_text(label_status, "SENSOR OFFLINE");
        lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        if (led_status != NULL) {
            lv_obj_set_style_bg_color(led_status, lv_color_hex(0xFF3B30), 0);
        }
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        if (led_status != NULL) {
            lv_obj_set_style_bg_color(led_status, lv_color_hex(0x31FF7A), 0);
        }
    }
}

void UI_UpdateDebug(uint32_t sps, uint32_t pps, uint8_t int_status,
                    uint8_t fifo_count, uint32_t red, uint32_t ir,
                    uint8_t part_id, uint8_t mode_config, uint8_t spo2_config,
                    uint8_t fifo_wr, uint8_t fifo_rd, uint8_t i2c_ok,
                    uint32_t task_mask, uint32_t ui_ticks, uint32_t ctrl_ticks)
{
    if (label_debug != NULL) {
        lv_label_set_text_fmt(label_debug,
                              "S%lu P%lu I%02x F%u ID%02x M%02x C%02x\nW%u R%u T%02lx U%lu C%lu %s",
                              (unsigned long)sps, (unsigned long)pps,
                              int_status, fifo_count, part_id,
                              mode_config, spo2_config, fifo_wr, fifo_rd,
                              (unsigned long)task_mask,
                              (unsigned long)ui_ticks,
                              (unsigned long)ctrl_ticks,
                              i2c_ok ? "OK" : "I2C!");
        lv_obj_invalidate(label_debug);
    }
    if (label_red_raw != NULL) {
        lv_label_set_text_fmt(label_red_raw, "RED: %lu", (unsigned long)red);
        lv_obj_invalidate(label_red_raw);
    }
    if (label_ir_raw  != NULL) {
        lv_label_set_text_fmt(label_ir_raw,  "IR : %lu", (unsigned long)ir);
        lv_obj_invalidate(label_ir_raw);
    }
}
