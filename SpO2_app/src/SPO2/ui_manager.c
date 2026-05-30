#include "ui_manager.h"
#include "../third_party/lvgl/lvgl.h"

static lv_obj_t * chart;
static lv_chart_series_t * ser_red;
static lv_chart_series_t * ser_ir;
static lv_obj_t * label_spo2;
static lv_obj_t * label_hr;
static lv_obj_t * label_status;

void UI_Init(void) {
    /* Main Screen Layout */
    lv_obj_t * screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_palette_main(LV_PALETTE_GREY), 0);

    /* Left Side: Waveform Chart */
    chart = lv_chart_create(screen);
    lv_obj_set_size(chart, 800, 500);
    lv_obj_align(chart, LV_ALIGN_LEFT_MID, 20, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR); // Medical style!
    lv_chart_set_point_count(chart, 200);

    ser_red = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    ser_ir  = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);

    /* Right Side: Big Numbers */
    label_spo2 = lv_label_create(screen);
    lv_obj_align(label_spo2, LV_ALIGN_TOP_RIGHT, -50, 100);
    lv_label_set_text(label_spo2, "SpO2: --%");
    // Set thin font style here if available

    label_hr = lv_label_create(screen);
    lv_obj_align(label_hr, LV_ALIGN_TOP_RIGHT, -50, 250);
    lv_label_set_text(label_hr, "HR: --");

    /* Status Label */
    label_status = lv_label_create(screen);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(label_status, "");
    lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void UI_UpdateMetrics(float spo2, int hr) {
    if (spo2 > 0) {
        lv_label_set_text_fmt(label_spo2, "SpO2: %.1f%%", spo2);
        if (spo2 < 90) lv_obj_set_style_text_color(label_spo2, lv_palette_main(LV_PALETTE_RED), 0);
        else lv_obj_set_style_text_color(label_spo2, lv_palette_main(LV_PALETTE_GREEN), 0);
    }
    
    if (hr > 0) {
        lv_label_set_text_fmt(label_hr, "HR: %d", hr);
    }
}

void UI_PushWaveform(uint32_t red, uint32_t ir) {
    // Normalizing 18-bit data to chart range (e.g. 0-100)
    lv_chart_set_next_value(chart, ser_red, (red >> 10) & 0x3FF);
    lv_chart_set_next_value(chart, ser_ir, (ir >> 10) & 0x3FF);
}

void UI_UpdateSensorStatus(bool online) {
    if (!online) {
        lv_label_set_text(label_status, "SENSOR OFFLINE");
        lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_RED), 0);
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
    }
}
