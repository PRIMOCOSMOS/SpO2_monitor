#ifndef SPO2_CALC_H
#define SPO2_CALC_H

#include <stdint.h>

/** Legacy raw-only wrapper. */
float SPO2_Calculate(uint32_t red, uint32_t ir);

/**
 * @brief SpO2 ratio-of-ratios using raw DC and filtered/software AC.
 * @param red_raw raw 18-bit RED ADC sample
 * @param ir_raw raw 18-bit IR ADC sample
 * @param red_ac signed AC component for RED
 * @param ir_ac signed AC component for IR
 * @return stable SpO2 percentage; 0 until locked/no finger/saturated.
 */
float SPO2_CalculateFiltered(uint32_t red_raw, uint32_t ir_raw,
                             int32_t red_ac, int32_t ir_ac);

#endif
