#ifndef SPO2_CALC_H
#define SPO2_CALC_H

#include <math.h>

/**
 * @brief Simple Ratio-of-Ratios SpO2 calculation
 * R = (AC_red/DC_red) / (AC_ir/DC_ir)
 * SpO2 = 110 - 25 * R
 */
float SPO2_Calculate(uint32_t red, uint32_t ir);

#endif
