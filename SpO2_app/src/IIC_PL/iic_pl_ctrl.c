#include "iic_pl_ctrl.h"
#include "xparameters.h"

static XIic IicInstance;

XStatus IIC_PL_Init(void) {
    XIic_Config *ConfigPtr;
    XStatus Status;

    ConfigPtr = XIic_LookupConfig(XPAR_AXI_IIC_0_BASEADDR);
    if (ConfigPtr == NULL) return XST_FAILURE;

    Status = XIic_CfgInitialize(&IicInstance, ConfigPtr, ConfigPtr->BaseAddress);
    if (Status != XST_SUCCESS) return Status;

    Status = XIic_Start(&IicInstance);
    return Status;
}

/*
 * XIic_Send/XIic_Recv return the number of bytes transferred, not XST_SUCCESS.
 * Normalize all IIC helpers to XST_SUCCESS/XST_FAILURE so upper layers do not
 * silently treat short transfers as valid samples.
 */
XStatus IIC_PL_WriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    unsigned sent = XIic_Send(IicInstance.BaseAddress, addr, buf, sizeof(buf), XIIC_STOP);
    return (sent == sizeof(buf)) ? XST_SUCCESS : XST_FAILURE;
}

XStatus IIC_PL_ReadReg(uint8_t addr, uint8_t reg, uint8_t *val) {
    unsigned sent = XIic_Send(IicInstance.BaseAddress, addr, &reg, 1, XIIC_REPEATED_START);
    if (sent != 1) return XST_FAILURE;

    unsigned received = XIic_Recv(IicInstance.BaseAddress, addr, val, 1, XIIC_STOP);
    return (received == 1) ? XST_SUCCESS : XST_FAILURE;
}

XStatus IIC_PL_ReadBuf(uint8_t addr, uint8_t reg, uint8_t *buf, int len) {
    if (len <= 0) return XST_FAILURE;

    unsigned sent = XIic_Send(IicInstance.BaseAddress, addr, &reg, 1, XIIC_REPEATED_START);
    if (sent != 1) return XST_FAILURE;

    unsigned received = XIic_Recv(IicInstance.BaseAddress, addr, buf, (unsigned)len, XIIC_STOP);
    return (received == (unsigned)len) ? XST_SUCCESS : XST_FAILURE;
}
