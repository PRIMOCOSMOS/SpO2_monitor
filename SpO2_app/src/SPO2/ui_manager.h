#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/* UI Module Initialization */
void UI_Init(void);

/* Update numerical values */
void UI_UpdateMetrics(float spo2, int hr);

/* Push waveform samples. UI_PushFilteredWaveform is the authoritative monitor
 * waveform path and expects FIR-filtered signed AC samples. UI_PushWaveform is
 * retained only as a raw compatibility wrapper. */
void UI_PushWaveform(uint32_t red, uint32_t ir);
void UI_PushFilteredWaveform(int32_t red_ac, int32_t ir_ac);

/* Show/Hide error overlays */
void UI_UpdateSensorStatus(bool online);

/* On-screen debug line for XSDB/no-UART bring-up */
void UI_UpdateDebug(uint32_t sps, uint32_t pps, uint8_t int_status,
                    uint8_t fifo_count, uint32_t red, uint32_t ir,
                    uint8_t part_id, uint8_t mode_config, uint8_t spo2_config,
                    uint8_t fifo_wr, uint8_t fifo_rd, uint8_t i2c_ok,
                    uint32_t task_mask, uint32_t ui_ticks, uint32_t ctrl_ticks);

#endif
