#include "hr_calc.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

#define PEAK_THRESHOLD 1000
#define MIN_TIME_BETWEEN_BEATS 400 

static uint32_t last_peak_time = 0;
static uint32_t last_sample = 0;

int HR_Calculate(uint32_t ir_sample) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int hr = 0;

    if (ir_sample < last_sample && last_sample > PEAK_THRESHOLD) {
        if (now - last_peak_time > MIN_TIME_BETWEEN_BEATS) {
            uint32_t interval = now - last_peak_time;
            if (interval > 0) {
                hr = 60000 / interval;
                last_peak_time = now;
            }
        }
    }
    last_sample = ir_sample;
    return hr;
}
