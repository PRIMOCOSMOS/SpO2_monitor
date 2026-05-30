#ifndef DMA_CTRL_H
#define DMA_CTRL_H

#include "xaxidma.h"
#include "xstatus.h"

XStatus DMA_Init(void);
XStatus DMA_Transfer(void* DestAddr, int Length);

#endif
