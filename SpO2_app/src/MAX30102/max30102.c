#include "max30102.h"
#include "../IIC_PL/iic_pl_ctrl.h"
#include "xil_printf.h"

#define MAX30102_ADDR       0x57
#define REG_INTR_STATUS_1   0x00
#define REG_FIFO_WR_PTR     0x04
#define REG_FIFO_DATA       0x07
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C
#define REG_LED2_PA         0x0D

XStatus MAX30102_Init(MAX30102_Config_t *config) {
    // Reset sequence
    IIC_PL_WriteReg(MAX30102_ADDR, REG_MODE_CONFIG, 0x40); 
    
    // SpO2 Mode (Red + IR)
    IIC_PL_WriteReg(MAX30102_ADDR, REG_MODE_CONFIG, 0x03);
    
    // Configuration: 411us pulse width, 18-bit ADC resolution
    IIC_PL_WriteReg(MAX30102_ADDR, REG_SPO2_CONFIG, (config->sample_rate << 2) | 0x03);
    
    // LED Pulse Amplitudes
    IIC_PL_WriteReg(MAX30102_ADDR, REG_LED1_PA, config->led_current);
    IIC_PL_WriteReg(MAX30102_ADDR, REG_LED2_PA, config->led_current);

    // Clear FIFO pointers
    IIC_PL_WriteReg(MAX30102_ADDR, REG_FIFO_WR_PTR, 0x00);
    
    return MAX30102_CheckStatus() ? XST_SUCCESS : XST_FAILURE;
}

XStatus MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir) {
    uint8_t buffer[6];
    
    // Read 6 bytes from FIFO (3 bytes Red, 3 bytes IR)
    if (IIC_PL_ReadBuf(MAX30102_ADDR, REG_FIFO_DATA, buffer, 6) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    // Masking 18 bits as per hardware spec
    *red = ((uint32_t)buffer[0] << 16 | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2]) & 0x3FFFF;
    *ir  = ((uint32_t)buffer[3] << 16 | (uint32_t)buffer[4] << 8 | (uint32_t)buffer[5]) & 0x3FFFF;

    return XST_SUCCESS;
}

bool MAX30102_CheckStatus(void) {
    uint8_t id;
    // Part ID register for MAX30102 is 0xFF, should return 0x11
    if (IIC_PL_ReadReg(MAX30102_ADDR, 0xFF, &id) == XST_SUCCESS) {
        return (id == 0x11);
    }
    return false;
}
