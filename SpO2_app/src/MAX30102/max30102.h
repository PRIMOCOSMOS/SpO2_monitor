#ifndef MAX30102_H
#define MAX30102_H

#include "xil_types.h"
#include "xstatus.h"

typedef struct {
    uint32_t red;
    uint32_t ir;
} ppg_data_t;

typedef enum {
    MAX30102_SR_50 = 0x00,
    MAX30102_SR_100 = 0x01,
    MAX30102_SR_400 = 0x03
} MAX30102_SR_t;

typedef enum {
    MAX30102_LED_CUR_0MA = 0x00,
    MAX30102_LED_CUR_50MA = 0xFF
} MAX30102_LED_CUR_t;

typedef struct {
    MAX30102_SR_t sample_rate;
    MAX30102_LED_CUR_t led_current;
} MAX30102_Config_t;

/* Public API */
XStatus MAX30102_Init(MAX30102_Config_t *config);
XStatus MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir);
bool MAX30102_CheckStatus(void);

#endif
