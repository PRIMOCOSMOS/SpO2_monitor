#include "ui_manager.h"
#include "../third_party/lvgl/lvgl.h"
#include "../display_ctrl/display_ctrl.h"
#include "xil_cache.h"

/* UI Objects */
static lv_obj_t * chart;
static lv_chart_series_t * ser_red;
static lv_chart_series_t * ser_ir;
static lv_obj_t * label_spo2;
static lv_obj_t * label_hr;
static lv_obj_t * label_status;

/* Display Flush Callback */
static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    // 获取硬件显存的基地址 (在 main.c 中通过 pFrames[0] 传入)
    uint32_t * fb = (uint32_t *)lv_display_get_user_data(disp);
    uint32_t * draw_buf = (uint32_t *)px_map;

    int32_t x, y;
    int32_t w = lv_area_get_width(area);
    
    /* 逐行将 LVGL 渲染的 Partial Buffer 拷贝到硬件 Framebuffer */
    for(y = area->y1; y <= area->y2; y++) {
        // 计算目标显存的行起始偏移
        uint32_t * dst = &fb[y * 800 + area->x1];
        for(x = area->x1; x <= area->x2; x++) {
            *dst++ = *draw_buf++;
        }
    }

    /* 必须刷新 Cache，否则 VDMA 读取的是旧数据或全 0（导致白屏/黑屏） */
    Xil_DCacheFlushRange((UINTPTR)&fb[area->y1 * 800], (area->y2 - area->y1 + 1) * 800 * 4);
    
    lv_display_flush_ready(disp);
}

void UI_Init(void) {
    /* 1. 创建 800x480 显示对象 */
    lv_display_t * disp = lv_display_create(800, 480);
    
    /* 2. 分配绘图缓冲区 (位于 DDR) */
    static uint32_t draw_buf[800 * 60]; 
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    /* 3. 获取 main.c 中分配的物理帧地址 */
    extern uint8_t *pFrames[];
    lv_display_set_user_data(disp, pFrames[0]); 
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_palette_main(LV_PALETTE_GREY), 0);

    /* Waveform Chart - Left Side (800x480 Optimization) */
    chart = lv_chart_create(screen);
    lv_obj_set_size(chart, 550, 350);
    lv_obj_align(chart, LV_ALIGN_LEFT_MID, 15, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(chart, 150);

    ser_red = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    ser_ir  = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);

    /* Numerical Values - Right Side */
    label_spo2 = lv_label_create(screen);
    lv_obj_align(label_spo2, LV_ALIGN_TOP_RIGHT, -30, 80);
    lv_label_set_text(label_spo2, "SpO2: --%");
    lv_obj_set_style_text_font(label_spo2, &lv_font_montserrat_24, 0);

    label_hr = lv_label_create(screen);
    lv_obj_align(label_hr, LV_ALIGN_TOP_RIGHT, -30, 180);
    lv_label_set_text(label_hr, "HR: --");
    lv_obj_set_style_text_font(label_hr, &lv_font_montserrat_24, 0);

    /* Status Overlay */
    label_status = lv_label_create(screen);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_48, 0);
    lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void UI_UpdateMetrics(float spo2, int hr) {
    if (spo2 > 0) {
        lv_label_set_text_fmt(label_spo2, "SpO2: %.1f%%", (double)spo2);
        if (spo2 < 90) lv_obj_set_style_text_color(label_spo2, lv_palette_main(LV_PALETTE_RED), 0);
        else lv_obj_set_style_text_color(label_spo2, lv_palette_main(LV_PALETTE_GREEN), 0);
    }
    
    if (hr > 0) {
        lv_label_set_text_fmt(label_hr, "HR: %d", hr);
    }
}

void UI_PushWaveform(uint32_t red, uint32_t ir) {
    // Basic scaling for display (18-bit to chart range)
    lv_chart_set_next_value(chart, ser_red, (lv_coord_t)((red >> 10) & 0x3FF));
    lv_chart_set_next_value(chart, ser_ir, (lv_coord_t)((ir >> 10) & 0x3FF));
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
