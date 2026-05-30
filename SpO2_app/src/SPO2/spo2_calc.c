#include "spo2_calc.h"

#define BUFFER_SIZE 100
static float red_buffer[BUFFER_SIZE];
static float ir_buffer[BUFFER_SIZE];
static int head = 0;

float SPO2_Calculate(uint32_t red, uint32_t ir) {
    // Add to circular buffer
    red_buffer[head] = (float)red;
    ir_buffer[head] = (float)ir;
    head = (head + 1) % BUFFER_SIZE;

    // Estimate DC (Average) and AC (Peak-to-Peak)
    float red_min = 999999, red_max = 0, red_avg = 0;
    float ir_min = 999999, ir_max = 0, ir_avg = 0;

    for(int i=0; i<BUFFER_SIZE; i++) {
        red_avg += red_buffer[i];
        if(red_buffer[i] < red_min) red_min = red_buffer[i];
        if(red_buffer[i] > red_max) red_max = red_buffer[i];

        ir_avg += ir_buffer[i];
        if(ir_buffer[i] < ir_min) ir_min = ir_buffer[i];
        if(ir_buffer[i] > ir_max) ir_max = ir_buffer[i];
    }
    red_avg /= BUFFER_SIZE;
    ir_avg /= BUFFER_SIZE;

    float red_ac = red_max - red_min;
    float ir_ac = ir_max - ir_min;

    if (ir_avg > 0 && red_avg > 0 && ir_ac > 0) {
        float R = (red_ac / red_avg) / (ir_ac / ir_avg);
        float spo2 = 110.0 - 25.0 * R; // Simplified linear approximation
        if (spo2 > 100) spo2 = 100;
        if (spo2 < 50) spo2 = 50;
        return spo2;
    }

    return 0.0;
}
