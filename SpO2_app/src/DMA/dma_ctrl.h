#ifndef DMA_CTRL_H
#define DMA_CTRL_H

#include "xaxidma.h"
#include "xstatus.h"
#include <stdint.h>

#define DMA_FIR_CH_RED 0U
#define DMA_FIR_CH_IR  1U

XStatus DMA_Init(void);

/* Legacy helper: one S2MM transfer on DMA0. */
XStatus DMA_Transfer(void* DestAddr, int Length);

/* Pass a vector through the PL FIR chain:
 * DDR -> AXI DMA MM2S -> FIR Compiler -> AXI DMA S2MM -> DDR.
 * channel 0 uses axi_dma_0/fir_compiler_0; channel 1 uses axi_dma_1/fir_compiler_1.
 */
XStatus DMA_FIR_FilterChannel(uint8_t channel, const int32_t *input,
                              int32_t *output, uint32_t sample_count);
XStatus DMA_FIR_FilterPair(const int32_t *red_in, int32_t *red_out,
                           const int32_t *ir_in,  int32_t *ir_out,
                           uint32_t sample_count);

uint32_t DMA_GetReadyMask(void);
uint32_t DMA_GetErrorMask(void);

#endif
