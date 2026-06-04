#include "int_controller.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "portmacro.h"
#include "xparameters.h"
#include "xscugic_hw.h"
#include "xil_io.h"

extern SemaphoreHandle_t xSensorDataReady;

static void set_gic_trigger_level_high(uint32_t IntId)
{
    UINTPTR dist_base = XPAR_XSCUGIC_0_BASEADDR;
    uint32_t cfg_off = XSCUGIC_INT_CFG_OFFSET_CALC(IntId);
    uint32_t shift = (IntId % 16U) * 2U;
    uint32_t v = XScuGic_ReadReg(dist_base, cfg_off);
    v &= ~(XSCUGIC_INT_CFG_MASK << shift);
    v |= (0x1U << shift); /* 01: level-sensitive, active high */
    XScuGic_WriteReg(dist_base, cfg_off, v);

    uint32_t prio_off = XSCUGIC_PRIORITY_OFFSET_CALC(IntId);
    shift = (IntId % 4U) * 8U;
    v = XScuGic_ReadReg(dist_base, prio_off);
    v &= ~(0xFFU << shift);
    v |= (0xA0U << shift);
    XScuGic_WriteReg(dist_base, prio_off, v);
}

void MAX30102_Int_Handler(void *CallbackRef) {
    (void)CallbackRef;

    /* No I2C in ISR.  Just wake the sensor task; it clears MAX30102 status and
     * drains FIFO in task context. */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xSensorDataReady, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

XStatus Setup_Interrupt_System(XScuGic *GicInstPtr, uint32_t IntId) {
    (void)GicInstPtr;

    /*
     * Install through the Xilinx FreeRTOS port.  Do NOT reinitialize/register a
     * separate GIC instance after the scheduler has started; that can break the
     * RTOS tick/vector state.
     *
     * Also do not reference the port's internal xInterruptController symbol
     * directly.  Some BSP/SDT builds hide or replace it, producing a linker
     * error such as:
     *   undefined reference to `xInterruptController'
     *   collect2.exe: error: ld returned 1 exit status
     *
     * The HWH/SDT already describes pl_ps_irq1[0] as LEVEL_HIGH after the PL NOT
     * gate.  If a future BSP still needs explicit trigger setup, do it in the
     * BSP/SDT interrupt metadata or add a board-specific hook there, not here.
     */
    BaseType_t ok = xPortInstallInterruptHandler((uint16_t)IntId,
                                                 (XInterruptHandler)MAX30102_Int_Handler,
                                                 NULL);
    if (ok != pdPASS) return XST_FAILURE;

    /* Program trigger type directly in the distributor.  This avoids linking to
     * the FreeRTOS port's internal GIC object while still fixing the real root
     * cause: pl_ps_irq1[0] is level-high after the PL NOT gate.
     */
    set_gic_trigger_level_high(IntId);

    vPortEnableInterrupt((uint16_t)IntId);
    return XST_SUCCESS;
}
