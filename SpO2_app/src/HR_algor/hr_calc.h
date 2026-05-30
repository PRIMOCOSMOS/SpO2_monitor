#ifndef HR_CALC_H
#define HR_CALC_H

#include <stdint.h>

/**
 * @brief Heart rate calculation based on PPG IR signal
 */
int HR_Calculate(uint32_t ir_sample);

#endif
