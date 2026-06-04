#ifndef HR_CALC_H
#define HR_CALC_H

#include <stdint.h>

/**
 * @brief Heart rate calculation based on raw IR sample (legacy wrapper).
 */
int HR_Calculate(uint32_t ir_sample);

/**
 * @brief Heart rate from filtered/AC IR PPG sample.
 * @param ir_ac signed AC component, positive on pulse upstroke/peak.
 * @param sample_rate_hz sample rate of input stream, e.g. 100.
 * @return last stable heart rate in bpm; 0 until locked.
 */
int HR_CalculateFiltered(int32_t ir_ac, uint32_t sample_rate_hz);

#endif
