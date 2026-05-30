#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>

void UI_Init(void);
void UI_UpdateMetrics(float spo2, int hr);
void UI_PushWaveform(uint32_t red, uint32_t ir);
void UI_UpdateSensorStatus(bool online);

#endif
