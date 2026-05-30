#ifndef IIC_PL_CTRL_H
#define IIC_PL_CTRL_H

#include "xiic.h"
#include "xstatus.h"

XStatus IIC_PL_Init(void);
XStatus IIC_PL_WriteReg(uint8_t addr, uint8_t reg, uint8_t val);
XStatus IIC_PL_ReadReg(uint8_t addr, uint8_t reg, uint8_t *val);
XStatus IIC_PL_ReadBuf(uint8_t addr, uint8_t reg, uint8_t *buf, int len);

#endif
