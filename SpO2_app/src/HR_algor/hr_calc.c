#include "hr_calc.h"
#include <stdint.h>

#define HR_DEFAULT_FS_HZ 100U
#define HR_MIN_BPM       35U
#define HR_MAX_BPM       210U
#define HR_LOCK_BEATS    2U

static uint32_t sample_index = 0;
static uint32_t last_event_index = 0;
static uint32_t valid_count = 0;
static int last_hr = 0;

static float dc_track = 0.0f;
static float prev = 0.0f;
static float prev2 = 0.0f;
static float abs_track = 20.0f;
static int armed_pos = 0;
static int armed_neg = 0;

static int accept_event(uint32_t at, uint32_t fs)
{
    uint32_t min_interval = (fs * 60U) / HR_MAX_BPM;
    uint32_t max_interval = (fs * 60U) / HR_MIN_BPM;
    if (min_interval < 1U) min_interval = 1U;

    if (last_event_index == 0U) {
        last_event_index = at;
        return 0;
    }

    uint32_t interval = at - last_event_index;
    if (interval < min_interval) return 0;
    if (interval > max_interval) {
        valid_count = 0;
        last_hr = 0;
        last_event_index = at;
        return 0;
    }

    int bpm = (int)((60U * fs + interval / 2U) / interval);
    if (valid_count < HR_LOCK_BEATS) {
        valid_count++;
        last_hr = bpm;
    } else {
        last_hr = (last_hr * 3 + bpm) / 4;
    }
    last_event_index = at;
    return 1;
}

int HR_CalculateFiltered(int32_t ir_ac, uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U) sample_rate_hz = HR_DEFAULT_FS_HZ;
    sample_index++;

    float x = (float)ir_ac;
    float ax = (x >= 0.0f) ? x : -x;
    abs_track = 0.992f * abs_track + 0.008f * ax;
    float thr = abs_track * 0.35f;
    if (thr < 5.0f) thr = 5.0f;

    uint32_t max_interval = (sample_rate_hz * 60U) / HR_MIN_BPM;

    /* Hysteretic zero-cross detector: more robust than local peak detection for
     * PL band-pass output and works for either polarity. */
    if (x > thr) armed_pos = 1;
    if (x < -thr) armed_neg = 1;

    if (armed_neg && prev <= 0.0f && x > 0.0f && ax > thr) {
        (void)accept_event(sample_index, sample_rate_hz);
        armed_neg = 0;
    } else if (armed_pos && prev >= 0.0f && x < 0.0f && ax > thr) {
        /* Inverted morphology fallback. */
        (void)accept_event(sample_index, sample_rate_hz);
        armed_pos = 0;
    }

    /* Backup local-extrema detector for very asymmetric waveforms. */
    if ((prev > prev2 && prev >= x && prev > 2.0f * thr) ||
        (prev < prev2 && prev <= x && -prev > 2.0f * thr)) {
        (void)accept_event(sample_index - 1U, sample_rate_hz);
    }

    if (last_event_index != 0U && (sample_index - last_event_index) > max_interval) {
        valid_count = 0;
        last_hr = 0;
    }

    prev2 = prev;
    prev = x;
    return (valid_count >= HR_LOCK_BEATS) ? last_hr : 0;
}

int HR_Calculate(uint32_t ir_sample)
{
    float sample = (float)ir_sample;
    if (dc_track <= 1.0f) dc_track = sample;
    dc_track = 0.99f * dc_track + 0.01f * sample;
    return HR_CalculateFiltered((int32_t)(sample - dc_track), HR_DEFAULT_FS_HZ);
}
