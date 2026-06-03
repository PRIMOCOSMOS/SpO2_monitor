#include "int_controller.h"
#include "../MAX30102/max30102.h"
#include "FreeRTOS.h"
#include "semphr.h"

extern SemaphoreHandle_t xSensorDataReady;

void MAX30102_Int_Handler(void *CallbackRef) {
    // Clear interrupt on sensor
    MAX30102_ReadInterruptStatus();

    // Signal task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xSensorDataReady, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

XStatus Setup_Interrupt_System(XScuGic *GicInstPtr, uint32_t IntId) {
    XScuGic_Config *IntcConfig;
    XStatus Status;

    IntcConfig = XScuGic_LookupConfig(XPAR_XSCUGIC_0_BASEADDR);
    if (NULL == IntcConfig) return XST_FAILURE;

    Status = XScuGic_CfgInitialize(GicInstPtr, IntcConfig, IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) return Status;

    /* MAX30102 INT 为 Active-Low，但 PL 端加了非门，到 PS 端变为 Active-High / 上升沿触发 */
    XScuGic_SetPriorityTriggerType(GicInstPtr, IntId, 0xA0, 0x3);
    Status = XScuGic_Connect(GicInstPtr, IntId, (Xil_ExceptionHandler)MAX30102_Int_Handler, NULL);
    if (Status != XST_SUCCESS) return Status;

    XScuGic_Enable(GicInstPtr, IntId);

    // Register GIC with exception table
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, GicInstPtr);
    Xil_ExceptionEnable();

    return XST_SUCCESS;
}
