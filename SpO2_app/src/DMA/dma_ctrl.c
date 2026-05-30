#include "dma_ctrl.h"
#include "xparameters.h"

static XAxiDma AxiDmaInstance;

XStatus DMA_Init(void) {
    XAxiDma_Config *CfgPtr;
    CfgPtr = XAxiDma_LookupConfig(XPAR_AXI_DMA_0_BASEADDR);
    if (!CfgPtr) return XST_FAILURE;

    XStatus Status = XAxiDma_CfgInitialize(&AxiDmaInstance, CfgPtr);
    if (Status != XST_SUCCESS) return Status;

    XAxiDma_IntrDisable(&AxiDmaInstance, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    return XST_SUCCESS;
}

XStatus DMA_Transfer(void* DestAddr, int Length) {
    Xil_DCacheFlushRange((UINTPTR)DestAddr, Length);
    return XAxiDma_SimpleTransfer(&AxiDmaInstance, (UINTPTR)DestAddr, Length, XAXIDMA_DEVICE_TO_DMA);
}
