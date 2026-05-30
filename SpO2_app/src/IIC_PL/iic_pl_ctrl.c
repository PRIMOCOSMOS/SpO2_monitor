#include "iic_pl_ctrl.h"
#include "xparameters.h"

static XIic IicInstance;

XStatus IIC_PL_Init(void) {
    XIic_Config *ConfigPtr;
    XStatus Status;

    ConfigPtr = XIic_LookupConfig(XPAR_AXI_IIC_0_BASEADDR); // Assuming standard naming
    if (ConfigPtr == NULL) return XST_FAILURE;

    Status = XIic_CfgInitialize(&IicInstance, ConfigPtr, ConfigPtr->BaseAddress);
    if (Status != XST_SUCCESS) return Status;

    XIic_Start(&IicInstance);
    return XST_SUCCESS;
}

XStatus IIC_PL_WriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return XIic_Send(IicInstance.BaseAddress, addr, buf, 2, XIIC_STOP);
}

XStatus IIC_PL_ReadReg(uint8_t addr, uint8_t reg, uint8_t *val) {
    XIic_Send(IicInstance.BaseAddress, addr, &reg, 1, XIIC_REPEATED_START);
    return XIic_Recv(IicInstance.BaseAddress, addr, val, 1, XIIC_STOP);
}

XStatus IIC_PL_ReadBuf(uint8_t addr, uint8_t reg, uint8_t *buf, int len) {
    XIic_Send(IicInstance.BaseAddress, addr, &reg, 1, XIIC_REPEATED_START);
    return XIic_Recv(IicInstance.BaseAddress, addr, buf, len, XIIC_STOP);
}
