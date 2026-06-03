#include "ui_manager.h"
#include "../third_party/lvgl/lvgl.h"
#include "../display_ctrl/display_ctrl.h"
#include "xil_cache.h"

/* Display geometry matching VDMA / main.c */
#define DISPLAY_WIDTH  800
#define DISPLAY_HEIGHT 480

/* Externals from main.c */
extern DisplayCtrl gDispCtrl;
extern uint8_t *pFrames[];

/* VDMA triple buffer cycling */
static uint32_t write_idx = 0;

/* LVGL v9 display handle */
static lv_display_t * disp;

/* Full-screen 32-bit render buffer for LVGL (ARGB8888 / XRGB8888) */
static uint32_t lv_buf[DISPLAY_WIDTH * DISPLAY_HEIGHT];

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
        int32_t src_y = y;
        int32_t dst_y = area->y1 + y;
        uint32_t * src_row = src + src_y * w;
        uint8_t  * dst_row = dst + dst_y * (DISPLAY_WIDTH * 3) + area->x1 * 3;

        for (int32_t x = 0; x < w; x++) {
            uint32_t pixel = src_row[x];        /* 0xAARRGGBB or 0xXXRRGGBB */
            dst_row[0] = (pixel >> 16) & 0xFF;  /* Red   */
            dst_row[1] = (pixel >> 8)  & 0xFF;  /* Green */
            dst_row[2] = pixel & 0xFF;          /* Blue  */
            dst_row += 3;
        }
    }

    /* Flush entire frame so VDMA sees the new data */
    Xil_DCacheFlushRange((UINTPTR)dst, DISPLAY_HEIGHT * DISPLAY_WIDTH * 3);

    /* Advance VDMA to the freshly rendered frame */
    DisplayChangeFrame(&gDispCtrl, write_idx);
    write_idx = (write_idx + 1) % DISPLAY_NUM_FRAMES;

    lv_display_flush_ready(display);
}

/* UI Objects */
static lv_obj_t * chart;
static lv_chart_series_t * ser_red;
static lv_chart_series_t * ser_ir;
static lv_obj_t * label_spo2;
static lv_obj_t * label_hr;
static lv_obj_t * label_status;

void UI_Init(void) {
    /* 1. Create display and attach flush + full-screen buffer */
    disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_buffers(disp, lv_buf, NULL, sizeof(lv_buf), LV_DISPLAY_RENDER_MODE_FULL);

    /* 2. Build UI */
    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x222222), 0);

    /* Waveform chart — left side */
    chart = lv_chart_create(screen);
    lv_obj_set_size(chart, 480, 400);
    lv_obj_align(chart, LV_ALIGN_LEFT_MID, 15, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 200);

    ser_red = lv_chart_add_series(chart, lv_color_hex(0xFF0000), LV_CHART_AXIS_PRIMARY_Y);
    ser_ir  = lv_chart_add_series(chart, lv_color_hex(0xFFA500), LV_CHART_AXIS_PRIMARY_Y);

    /* Numeric labels — right side */
    label_spo2 = lv_label_create(screen);
    lv_obj_align(label_spo2, LV_ALIGN_TOP_RIGHT, -30, 80);
    lv_label_set_text(label_spo2, "SpO2: --%");
    lv_obj_set_style_text_font(label_spo2, &lv_font_montserrat_24, 0);

    label_hr = lv_label_create(screen);
    lv_obj_align(label_hr, LV_ALIGN_TOP_RIGHT, -30, 180);
    lv_label_set_text(label_hr, "HR: --");
    lv_obj_set_style_text_font(label_hr, &lv_font_montserrat_24, 0);

    /* Status overlay — center, hidden by default */
    label_status = lv_label_create(screen);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_48, 0);
    lv_label_set_text(label_status, "");
    lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void UI_UpdateMetrics(float spo2, int hr) {
    if (spo2 > 0) {
        lv_label_set_text_fmt(label_spo2, "SpO2: %.1f%%", (double)spo2);
        if (spo2 < 90)
            lv_obj_set_style_text_color(label_spo2, lv_color_hex(0xFF0000), 0);
        else
            lv_obj_set_style_text_color(label_spo2, lv_color_hex(0x00FF00), 0);
    }

    if (hr > 0) {
        lv_label_set_text_fmt(label_hr, "HR: %d", hr);
    }
}

void UI_PushWaveform(uint32_t red, uint32_t ir) {
    /* Map 18-bit ADC value to chart range 0..1023 */
    lv_chart_set_next_value(chart, ser_red, (lv_coord_t)((red >> 8) & 0x3FF));
    lv_chart_set_next_value(chart, ser_ir,  (lv_coord_t)((ir  >> 8) & 0x3FF));
}

void UI_UpdateSensorStatus(bool online) {
    if (!online) {
        lv_label_set_text(label_status, "SENSOR OFFLINE");
        lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF0000), 0);
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
    }
}
