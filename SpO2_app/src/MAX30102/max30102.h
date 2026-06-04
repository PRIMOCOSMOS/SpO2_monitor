#ifndef MAX30102_H
#define MAX30102_H

#include "xil_types.h"
#include "xstatus.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX30102_FIFO_DEPTH 32U
#define MAX30102_EXPECTED_PART_ID 0x15U

/* Data Structure for Red and IR samples */
typedef struct {
    uint32_t red;
    uint32_t ir;
} ppg_data_t;

typedef struct {
    uint8_t int_status_1;
    uint8_t int_status_2;
    uint8_t int_enable_1;
    uint8_t int_enable_2;
    uint8_t fifo_wr_ptr;
    uint8_t fifo_ovf_counter;
    uint8_t fifo_rd_ptr;
    uint8_t fifo_config;
    uint8_t mode_config;
    uint8_t spo2_config;
    uint8_t led1_pa;
    uint8_t led2_pa;
    uint8_t revision_id;
    uint8_t part_id;
    uint8_t fifo_count;
    uint8_t i2c_ok;
} MAX30102_DebugRegs_t;

/* Configuration Enums */
typedef enum {
    MAX30102_SR_50  = 0x00,
    MAX30102_SR_100 = 0x01,
    MAX30102_SR_200 = 0x02,
    MAX30102_SR_400 = 0x03
} MAX30102_SR_t;

typedef enum {
    MAX30102_LED_CUR_0MA  = 0x00,
    MAX30102_LED_CUR_3MA  = 0x0F,
    MAX30102_LED_CUR_6MA  = 0x1F,
    MAX30102_LED_CUR_12MA = 0x3F,
    MAX30102_LED_CUR_25MA = 0x7F,
    MAX30102_LED_CUR_50MA = 0xFF
} MAX30102_LED_CUR_t;

typedef struct {
    MAX30102_SR_t sample_rate;
    MAX30102_LED_CUR_t led_current;
} MAX30102_Config_t;

/* Public API */
XStatus MAX30102_Init(MAX30102_Config_t *config);
XStatus MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir);
XStatus MAX30102_ReadSamples(ppg_data_t *samples, uint8_t max_samples, uint8_t *samples_read);
XStatus MAX30102_GetFifoSampleCount(uint8_t *count);
XStatus MAX30102_ReadDebugRegs(MAX30102_DebugRegs_t *dbg);
XStatus MAX30102_SetLEDCurrent(MAX30102_LED_CUR_t led_current);
bool MAX30102_CheckStatus(void);
uint8_t MAX30102_ReadInterruptStatus(void);

#endif
