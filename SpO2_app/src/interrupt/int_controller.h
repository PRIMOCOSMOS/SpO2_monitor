#ifndef INT_CONTROLLER_H
#define INT_CONTROLLER_H

#include "xscugic.h"
#include "xstatus.h"

XStatus Setup_Interrupt_System(XScuGic *GicInstPtr, uint32_t IntId);

#endif
