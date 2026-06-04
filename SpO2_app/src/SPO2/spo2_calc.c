#include "spo2_calc.h"
#include <stdint.h>

#define SPO2_FS_HZ          100U
#define SPO2_WIN_SIZE       400U   /* 4 s at 100 Hz */
#define SPO2_MIN_SAMPLES    120U
#define RAW_SAT_LIMIT       258000U
#define RAW_MIN_DC          1000U
#define MIN_AC_RMS          8.0f

static uint32_t red_raw_buf[SPO2_WIN_SIZE];
static uint32_t ir_raw_buf[SPO2_WIN_SIZE];
static int32_t red_ac_buf[SPO2_WIN_SIZE];
static int32_t ir_ac_buf[SPO2_WIN_SIZE];
static uint32_t head = 0;
static uint32_t filled = 0;
static float last_spo2 = 0.0f;

static float fast_sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;
    float g = (x > 1.0f) ? x : 1.0f;
    for (int i = 0; i < 8; ++i) g = 0.5f * (g + x / g);
    return g;
}

float SPO2_CalculateFiltered(uint32_t red_raw, uint32_t ir_raw,
                             int32_t red_ac, int32_t ir_ac)
{
    if (red_raw >= RAW_SAT_LIMIT || ir_raw >= RAW_SAT_LIMIT ||
        red_raw < RAW_MIN_DC || ir_raw < RAW_MIN_DC) {
        /* Do not immediately blank the display; decay slowly. */
        return last_spo2;
    }

    red_raw_buf[head] = red_raw;
    ir_raw_buf[head] = ir_raw;
    red_ac_buf[head] = red_ac;
    ir_ac_buf[head] = ir_ac;
    head = (head + 1U) % SPO2_WIN_SIZE;
    if (filled < SPO2_WIN_SIZE) filled++;

    if (filled < SPO2_MIN_SAMPLES) return last_spo2;

    uint64_t red_dc_sum = 0, ir_dc_sum = 0;
    double red_ac_sq = 0.0, ir_ac_sq = 0.0;

    for (uint32_t i = 0; i < filled; ++i) {
        red_dc_sum += red_raw_buf[i];
        ir_dc_sum += ir_raw_buf[i];
        red_ac_sq += (double)red_ac_buf[i] * (double)red_ac_buf[i];
        ir_ac_sq  += (double)ir_ac_buf[i]  * (double)ir_ac_buf[i];
    }

    float red_dc = (float)red_dc_sum / (float)filled;
    float ir_dc  = (float)ir_dc_sum  / (float)filled;
    float red_rms = fast_sqrtf((float)(red_ac_sq / (double)filled));
    float ir_rms  = fast_sqrtf((float)(ir_ac_sq  / (double)filled));

    if (red_dc < RAW_MIN_DC || ir_dc < RAW_MIN_DC || red_rms < MIN_AC_RMS || ir_rms < MIN_AC_RMS) {
        return last_spo2;
    }

    float R = (red_rms / red_dc) / (ir_rms / ir_dc);
    if (R < 0.25f || R > 2.2f) return last_spo2;

    /* Empirical calibration commonly used for MAX3010x modules.  Clamp to a
     * physiologically plausible display range for a demo monitor. */
    float spo2 = 110.0f - 25.0f * R;
    if (spo2 > 100.0f) spo2 = 100.0f;
    if (spo2 < 70.0f) spo2 = 70.0f;

    if (last_spo2 < 1.0f) last_spo2 = spo2;
    else last_spo2 = 0.92f * last_spo2 + 0.08f * spo2;

    return last_spo2;
}

float SPO2_Calculate(uint32_t red, uint32_t ir)
{
    static float red_dc = 0.0f, ir_dc = 0.0f;
    if (red_dc <= 1.0f) red_dc = (float)red;
    if (ir_dc <= 1.0f) ir_dc = (float)ir;
    red_dc = 0.99f * red_dc + 0.01f * (float)red;
    ir_dc  = 0.99f * ir_dc  + 0.01f * (float)ir;
    return SPO2_CalculateFiltered(red, ir, (int32_t)((float)red - red_dc), (int32_t)((float)ir - ir_dc));
}
