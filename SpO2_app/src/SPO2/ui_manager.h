#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/* UI Module Initialization */
void UI_Init(void);

/* Update numerical values */
void UI_UpdateMetrics(float spo2, int hr);

/* Push new sample to wave chart */
void UI_PushWaveform(uint32_t red, uint32_t ir);

/* Show/Hide error overlays */
void UI_UpdateSensorStatus(bool online);

#endif
