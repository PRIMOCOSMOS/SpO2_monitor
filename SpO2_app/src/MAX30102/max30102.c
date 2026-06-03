#include "max30102.h"
#include "../IIC_PL/iic_pl_ctrl.h"
#include "xil_printf.h"

#define MAX30102_ADDR       0x57
#define REG_INTR_STATUS_1   0x00
#define REG_INTR_ENABLE_1   0x02
#define REG_FIFO_WR_PTR     0x04
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C
#define REG_LED2_PA         0x0D

XStatus MAX30102_Init(MAX30102_Config_t *config) {
    // 1. Soft Reset
    IIC_PL_WriteReg(MAX30102_ADDR, REG_MODE_CONFIG, 0x40);
    
    // 2. Configure Interrupts: Enable A_FULL and PPG_RDY
    IIC_PL_WriteReg(MAX30102_ADDR, REG_INTR_ENABLE_1, 0xC0);

    // 3. FIFO Config: Rollover enabled, Almost Full at 17 samples
    IIC_PL_WriteReg(MAX30102_ADDR, REG_FIFO_CONFIG, 0x1F);

    // 4. SpO2 Mode (Red + IR)
    IIC_PL_WriteReg(MAX30102_ADDR, REG_MODE_CONFIG, 0x03);

    // 5. Configuration: SR and 18-bit ADC
    IIC_PL_WriteReg(MAX30102_ADDR, REG_SPO2_CONFIG, (config->sample_rate << 2) | 0x03);

    // 6. LED Pulse Amplitudes
    IIC_PL_WriteReg(MAX30102_ADDR, REG_LED1_PA, config->led_current);
    IIC_PL_WriteReg(MAX30102_ADDR, REG_LED2_PA, config->led_current);

    // 7. Clear FIFO pointers
    IIC_PL_WriteReg(MAX30102_ADDR, REG_FIFO_WR_PTR, 0x00);

    return MAX30102_CheckStatus() ? XST_SUCCESS : XST_FAILURE;
}

XStatus MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir) {
    uint8_t buffer[6];
    if (IIC_PL_ReadBuf(MAX30102_ADDR, REG_FIFO_DATA, buffer, 6) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    *red = ((uint32_t)buffer[0] << 16 | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2]) & 0x3FFFF;
    *ir  = ((uint32_t)buffer[3] << 16 | (uint32_t)buffer[4] << 8 | (uint32_t)buffer[5]) & 0x3FFFF;
    return XST_SUCCESS;
}

uint8_t MAX30102_ReadInterruptStatus(void) {
    uint8_t status;
    IIC_PL_ReadReg(MAX30102_ADDR, REG_INTR_STATUS_1, &status);
    return status;
}

bool MAX30102_CheckStatus(void) {
    uint8_t id;
    if (IIC_PL_ReadReg(MAX30102_ADDR, 0xFF, &id) == XST_SUCCESS) {
        return (id == 0x11);
    }
    return false;
}
