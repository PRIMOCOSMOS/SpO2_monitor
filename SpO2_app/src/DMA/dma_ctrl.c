#include "dma_ctrl.h"
#include "xparameters.h"
#include "xil_cache.h"
#include <stdint.h>

#define DMA_TIMEOUT_LOOPS 1000000U

static XAxiDma AxiDmaInstance[2];
static uint8_t DmaReady[2] = {0U, 0U};
static volatile uint32_t g_dma_ready_mask = 0U;
static volatile uint32_t g_dma_error_mask = 0U;

static UINTPTR dma_baseaddr(uint8_t channel)
{
    return (channel == DMA_FIR_CH_IR) ? XPAR_AXI_DMA_1_BASEADDR : XPAR_AXI_DMA_0_BASEADDR;
}

static XStatus dma_init_one(uint8_t channel)
{
    XAxiDma_Config *CfgPtr = XAxiDma_LookupConfig(dma_baseaddr(channel));
    if (CfgPtr == NULL) {
        g_dma_error_mask |= (1U << channel);
        return XST_FAILURE;
    }

    XStatus Status = XAxiDma_CfgInitialize(&AxiDmaInstance[channel], CfgPtr);
    if (Status != XST_SUCCESS) {
        g_dma_error_mask |= (1U << channel);
        return Status;
    }

    if (XAxiDma_HasSg(&AxiDmaInstance[channel])) {
        g_dma_error_mask |= (1U << (channel + 8U));
        return XST_FAILURE;
    }

    XAxiDma_IntrDisable(&AxiDmaInstance[channel], XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);
    XAxiDma_IntrDisable(&AxiDmaInstance[channel], XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

    DmaReady[channel] = 1U;
    g_dma_ready_mask |= (1U << channel);
    return XST_SUCCESS;
}

XStatus DMA_Init(void) {
    XStatus st0 = dma_init_one(DMA_FIR_CH_RED);
    XStatus st1 = dma_init_one(DMA_FIR_CH_IR);
    return (st0 == XST_SUCCESS && st1 == XST_SUCCESS) ? XST_SUCCESS : XST_FAILURE;
}

static XStatus wait_not_busy(XAxiDma *dma, int direction)
{
    uint32_t timeout = DMA_TIMEOUT_LOOPS;
    while (XAxiDma_Busy(dma, direction)) {
        if (--timeout == 0U) {
            return XST_FAILURE;
        }
    }
    return XST_SUCCESS;
}

XStatus DMA_FIR_FilterChannel(uint8_t channel, const int32_t *input,
                              int32_t *output, uint32_t sample_count)
{
    if (channel > DMA_FIR_CH_IR || input == NULL || output == NULL || sample_count == 0U) {
        return XST_FAILURE;
    }
    if (!DmaReady[channel]) {
        return XST_FAILURE;
    }

    const uint32_t len = sample_count * sizeof(int32_t);
    XAxiDma *dma = &AxiDmaInstance[channel];

    Xil_DCacheFlushRange((UINTPTR)input, len);
    Xil_DCacheFlushRange((UINTPTR)output, len);
    Xil_DCacheInvalidateRange((UINTPTR)output, len);

    /* Start receive first; otherwise the FIR output stream can back-pressure. */
    XStatus st = XAxiDma_SimpleTransfer(dma, (UINTPTR)output, len, XAXIDMA_DEVICE_TO_DMA);
    if (st != XST_SUCCESS) {
        g_dma_error_mask |= (1U << (channel + 16U));
        return st;
    }

    st = XAxiDma_SimpleTransfer(dma, (UINTPTR)input, len, XAXIDMA_DMA_TO_DEVICE);
    if (st != XST_SUCCESS) {
        g_dma_error_mask |= (1U << (channel + 18U));
        return st;
    }

    if (wait_not_busy(dma, XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS ||
        wait_not_busy(dma, XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        g_dma_error_mask |= (1U << (channel + 20U));
        return XST_FAILURE;
    }

    Xil_DCacheInvalidateRange((UINTPTR)output, len);
    return XST_SUCCESS;
}

XStatus DMA_FIR_FilterPair(const int32_t *red_in, int32_t *red_out,
                           const int32_t *ir_in,  int32_t *ir_out,
                           uint32_t sample_count)
{
    XStatus red_st = DMA_FIR_FilterChannel(DMA_FIR_CH_RED, red_in, red_out, sample_count);
    XStatus ir_st  = DMA_FIR_FilterChannel(DMA_FIR_CH_IR,  ir_in,  ir_out,  sample_count);
    return (red_st == XST_SUCCESS && ir_st == XST_SUCCESS) ? XST_SUCCESS : XST_FAILURE;
}

XStatus DMA_Transfer(void* DestAddr, int Length) {
    if (!DmaReady[DMA_FIR_CH_RED]) return XST_FAILURE;
    Xil_DCacheFlushRange((UINTPTR)DestAddr, (UINTPTR)Length);
    return XAxiDma_SimpleTransfer(&AxiDmaInstance[DMA_FIR_CH_RED],
                                  (UINTPTR)DestAddr, Length,
                                  XAXIDMA_DEVICE_TO_DMA);
}

uint32_t DMA_GetReadyMask(void)
{
    return g_dma_ready_mask;
}

uint32_t DMA_GetErrorMask(void)
{
    return g_dma_error_mask;
}
