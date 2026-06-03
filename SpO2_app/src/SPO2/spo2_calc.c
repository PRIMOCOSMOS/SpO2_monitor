#include "spo2_calc.h"
#include <stdint.h>

#define BUFFER_SIZE 100
static float red_buffer[BUFFER_SIZE];
static float ir_buffer[BUFFER_SIZE];
static int head = 0;

float SPO2_Calculate(uint32_t red, uint32_t ir) {
    red_buffer[head] = (float)red;
    ir_buffer[head] = (float)ir;
    head = (head + 1) % BUFFER_SIZE;

    float red_min = 1000000.0f, red_max = 0.0f, red_avg = 0.0f;
    float ir_min = 1000000.0f, ir_max = 0.0f, ir_avg = 0.0f;

    for(int i=0; i<BUFFER_SIZE; i++) {
        red_avg += red_buffer[i];
        if(red_buffer[i] < red_min) red_min = red_buffer[i];
        if(red_buffer[i] > red_max) red_max = red_buffer[i];

        ir_avg += ir_buffer[i];
        if(ir_buffer[i] < ir_min) ir_min = ir_buffer[i];
        if(ir_buffer[i] > ir_max) ir_max = ir_buffer[i];
    }
    red_avg /= (float)BUFFER_SIZE;
    ir_avg /= (float)BUFFER_SIZE;

    float red_ac = red_max - red_min;
    float ir_ac = ir_max - ir_min;

    if (ir_avg > 0.1f && red_avg > 0.1f && ir_ac > 0.1f) {
        float R = (red_ac / red_avg) / (ir_ac / ir_avg);
        float spo2 = 110.0f - 25.0f * R; 
        if (spo2 > 100.0f) spo2 = 100.0f;
        if (spo2 < 50.0f) spo2 = 50.0f;
        return spo2;
    }

    return 0.0f;
}
