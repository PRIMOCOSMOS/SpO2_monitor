#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xparameters_ps.h"
#include "xaxivdma.h"
#include "xscugic.h"
#include "xil_cache.h"
#include <string.h>
#include <stdint.h>

/* Project Includes */
#include "MAX30102/max30102.h"
#include "SPO2/spo2_calc.h"
#include "HR_algor/hr_calc.h"
#include "SPO2/ui_manager.h"
#include "display_ctrl/display_ctrl.h"
#include "PWM/ax_pwm.h"
#include "DMA/dma_ctrl.h"
#include "IIC_PL/iic_pl_ctrl.h"
#include "interrupt/int_controller.h"
#include "third_party/lvgl/lvgl.h"

/* Task Priorities */
#define TASK_SENSOR_PRIO    ( tskIDLE_PRIORITY + 5 )
#define TASK_PROC_PRIO      ( tskIDLE_PRIORITY + 4 )
#define TASK_UI_PRIO        ( tskIDLE_PRIORITY + 3 )
#define TASK_CTRL_PRIO      ( tskIDLE_PRIORITY + 2 )

/* FreeRTOS tick is 100 Hz in this BSP, so the minimum real task delay is
 * 1 tick = 10 ms. Do not use pdMS_TO_TICKS(1/5): it becomes 0 and creates
 * busy loops/assert-prone starvation. 100 Hz polling matches MAX30102_SR_100.
 */
#define SENSOR_POLL_MS          10U
#define SENSOR_POLL_TICKS       1U
#define SENSOR_OFFLINE_MS       3000U
#define MAX_FIFO_BURST_SAMPLES  32U
#define RAW_QUEUE_DEPTH         64U
#define WAVE_QUEUE_DEPTH        64U
/* Current PL AXI DMA is direct-register mode (C_INCLUDE_SG=0), so true
 * hardware cyclic BD rings are not available in this bitstream.  Use the
 * smallest practical software-managed micro-stream: 2 new samples plus 16
 * history samples per DMA transaction.  This gives one fresh FIR output every
 * 10 ms at 100 Hz while reducing FIR packet-boundary artifacts.
 */
#define FIR_BATCH_SAMPLES       2U
#define FIR_HISTORY_SAMPLES     16U
#define FIR_DMA_SAMPLES         (FIR_BATCH_SAMPLES + FIR_HISTORY_SAMPLES)

/* Keep MAX30102 acquisition in polling mode first. */
#define USE_MAX30102_IRQ        0
#define USE_SINGLE_MONITOR_TASK 1

/* Global Handles */
QueueHandle_t xRawDataQueue = NULL;
QueueHandle_t xWaveDataQueue = NULL;
SemaphoreHandle_t xSensorDataReady = NULL;

/* Sensor/debug flags (visible from XSDB; also displayed on-screen) */
volatile bool g_sensor_online = false;
volatile uint32_t g_dbg_sensor_sps = 0;
volatile uint32_t g_dbg_proc_pps = 0;
volatile uint32_t g_dbg_last_red = 0;
volatile uint32_t g_dbg_last_ir = 0;
volatile uint8_t  g_dbg_int_status = 0;
volatile uint8_t  g_dbg_fifo_count = 0;
volatile uint8_t  g_dbg_part_id = 0;
volatile uint8_t  g_dbg_mode_config = 0;
volatile uint8_t  g_dbg_spo2_config = 0;
volatile uint8_t  g_dbg_fifo_wr = 0;
volatile uint8_t  g_dbg_fifo_rd = 0;
volatile uint8_t  g_dbg_fifo_ovf = 0;
volatile uint8_t  g_dbg_led1 = 0;
volatile uint8_t  g_dbg_led2 = 0;
volatile uint8_t  g_dbg_i2c_ok = 0;
volatile uint32_t g_dbg_sensor_total = 0;
volatile uint32_t g_dbg_proc_total = 0;
volatile uint32_t g_dbg_ui_push_total = 0;
volatile uint32_t g_dbg_task_create_mask = 0;
volatile uint32_t g_dbg_task_fail_mask = 0;
volatile uint32_t g_dbg_ui_ticks = 0;
volatile uint32_t g_dbg_ctrl_ticks = 0;
volatile uint32_t g_dbg_sensor_ticks = 0;
volatile uint32_t g_dbg_boot_phase = 0;
volatile uint32_t g_dbg_dma_ready_mask = 0;
volatile uint32_t g_dbg_dma_error_mask = 0;
volatile uint32_t g_dbg_fir_ok_total = 0;
volatile uint32_t g_dbg_fir_fail_total = 0;
volatile uint32_t g_dbg_irq_enabled = 0;
volatile uint32_t g_dbg_irq_timeout = 0;
volatile uint32_t g_dbg_assert_line = 0;
volatile uintptr_t g_dbg_assert_file = 0;
volatile uint32_t g_dbg_malloc_failed = 0;
volatile uint32_t g_dbg_stack_overflow = 0;
volatile uintptr_t g_dbg_stack_task = 0;
volatile uintptr_t g_dbg_stack_name = 0;

static volatile float g_latest_spo2 = 0.0f;
static volatile int   g_latest_hr = 0;

/* Hardware Instances */
static XScuGic IntcInstance;
static XAxiVdma VdmaInstance;
DisplayCtrl gDispCtrl;   /* non-static: ui_manager.c calls DisplayChangeFrame */

/* Hardware Settings - Derived from HWH */
#define MAX30102_INT_ID     XPS_FPGA8_INT_ID
#define PWM_BASEADDR        XPAR_AX_PWM_0_BASEADDR
#define DYNCLK_BASEADDR     XPAR_AXI_DYNCLK_0_BASEADDR
#define VTC_BASEADDR        XPAR_V_TC_0_BASEADDR
#define VDMA_DEVICE_ID      XPAR_AXI_VDMA_0_BASEADDR

/* Framebuffers for 800x480 (WVGA) — 24-bit RGB888 to match VDMA tdata-width */
#define DISPLAY_WIDTH  800
#define DISPLAY_HEIGHT 480
#define STRIDE         (DISPLAY_WIDTH * 3)
uint8_t frameBuf[DISPLAY_NUM_FRAMES][DISPLAY_HEIGHT * STRIDE] __attribute__((aligned(256)));
uint8_t *pFrames[DISPLAY_NUM_FRAMES];

typedef struct {
    int32_t red_ac;
    int32_t ir_ac;
} fir_wave_sample_t;

/* FreeRTOS diagnostic hooks.  The BSP default vApplicationAssert() prints then
 * loops forever.  With no UART this hides the real cause, so override it and
 * expose file pointer/line in XSDB watch variables.
 */
void vApplicationAssert(const char *pcFile, uint32_t ulLine)
{
    g_dbg_assert_file = (uintptr_t)pcFile;
    g_dbg_assert_line = ulLine;
    taskDISABLE_INTERRUPTS();
    for (;;) { __asm__ volatile("wfe"); }
}

void vApplicationMallocFailedHook(void)
{
    g_dbg_malloc_failed++;
    g_dbg_assert_line = 0xBAD00001U;
    taskDISABLE_INTERRUPTS();
    for (;;) { __asm__ volatile("wfe"); }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    g_dbg_stack_overflow++;
    g_dbg_stack_task = (uintptr_t)xTask;
    g_dbg_stack_name = (uintptr_t)pcTaskName;
    g_dbg_assert_line = 0xBAD00002U;
    taskDISABLE_INTERRUPTS();
    for (;;) { __asm__ volatile("wfe"); }
}

/* Task Prototypes */
void vTaskSensor(void *pvParameters);
void vTaskProcessing(void *pvParameters);
void vTaskUI(void *pvParameters);
void vTaskControl(void *pvParameters);
void vTaskMonitor(void *pvParameters);

static void update_debug_from_regs(const MAX30102_DebugRegs_t *dbg)
{
    if (dbg == NULL) return;
    g_dbg_int_status = dbg->int_status_1;
    g_dbg_fifo_count = dbg->fifo_count;
    g_dbg_part_id = dbg->part_id;
    g_dbg_mode_config = dbg->mode_config;
    g_dbg_spo2_config = dbg->spo2_config;
    g_dbg_fifo_wr = dbg->fifo_wr_ptr;
    g_dbg_fifo_rd = dbg->fifo_rd_ptr;
    g_dbg_fifo_ovf = dbg->fifo_ovf_counter;
    g_dbg_led1 = dbg->led1_pa;
    g_dbg_led2 = dbg->led2_pa;
    g_dbg_i2c_ok = dbg->i2c_ok;
}

int main() {
    XStatus Status;
    xil_printf("\r\n--- SpO2 Monitor Start ---\r\n");

    for (int i = 0; i < DISPLAY_NUM_FRAMES; i++) {
        pFrames[i] = frameBuf[i];
        memset(pFrames[i], 0, DISPLAY_HEIGHT * STRIDE);
    }

    IIC_PL_Init();
    Status = DMA_Init();
    g_dbg_dma_ready_mask = DMA_GetReadyMask();
    g_dbg_dma_error_mask = DMA_GetErrorMask();
    if (Status != XST_SUCCESS) {
        xil_printf("[DMA] FIR DMA init failed, ready=0x%lx err=0x%lx\r\n",
                   (unsigned long)g_dbg_dma_ready_mask,
                   (unsigned long)g_dbg_dma_error_mask);
    }

    set_pwm_frequency(PWM_BASEADDR, 100000000, 1000.0f);
    set_pwm_duty(PWM_BASEADDR, 0.80f);

    XAxiVdma_Config *vdmaConfig = XAxiVdma_LookupConfig(VDMA_DEVICE_ID);
    if (vdmaConfig == NULL) {
        xil_printf("CRITICAL: XAxiVdma_LookupConfig failed!\r\n");
        while (1);
    }
    Status = XAxiVdma_CfgInitialize(&VdmaInstance, vdmaConfig, vdmaConfig->BaseAddress);
    if (Status != XST_SUCCESS) {
        xil_printf("CRITICAL: XAxiVdma_CfgInitialize failed (%d)!\r\n", Status);
        while (1);
    }

    Status = DisplayInitialize(&gDispCtrl, &VdmaInstance, VTC_BASEADDR, DYNCLK_BASEADDR, pFrames, STRIDE);
    if (Status != XST_SUCCESS) xil_printf("CRITICAL: DisplayInitialize failed (%d)!\r\n", Status);
    DisplaySetMode(&gDispCtrl, &VMODE_800x480);
    Status = DisplayStart(&gDispCtrl);
    if (Status != XST_SUCCESS) xil_printf("CRITICAL: DisplayStart failed (%d)!\r\n", Status);

    lv_init();
    UI_Init();
    g_dbg_boot_phase = 1;
    UI_UpdateDebug(0, 0, 0xA5, 0, 0, 0, 0xBD, 0xBD, 0xBD, 0, 0, 1, 0, 0, 0);
    lv_timer_handler();

#if USE_SINGLE_MONITOR_TASK
    xRawDataQueue = NULL;
    xWaveDataQueue = NULL;
    xSensorDataReady = NULL;
#else
    xRawDataQueue = xQueueCreate(RAW_QUEUE_DEPTH, sizeof(ppg_data_t));
    xWaveDataQueue = xQueueCreate(WAVE_QUEUE_DEPTH, sizeof(fir_wave_sample_t));
#if USE_MAX30102_IRQ
    xSensorDataReady = xSemaphoreCreateBinary();
#else
    xSensorDataReady = NULL;
#endif
#endif

#if USE_MAX30102_IRQ
    xil_printf("[INT] MAX30102 IRQ will be installed from Sensor task after device init.\r\n");
#else
    xil_printf("[INT] MAX30102 IRQ disabled; using fast 5 ms polling mode.\r\n");
#endif

#if USE_SINGLE_MONITOR_TASK
    if (xTaskCreate(vTaskMonitor, "Monitor", 4096, NULL, TASK_SENSOR_PRIO, NULL) == pdPASS)
        g_dbg_task_create_mask = 0x0FU;
    else
        g_dbg_task_fail_mask |= 0x0FU;
#else
    if (xTaskCreate(vTaskSensor, "Sensor", 1024, NULL, TASK_SENSOR_PRIO, NULL) == pdPASS)
        g_dbg_task_create_mask |= (1U << 0);
    else
        g_dbg_task_fail_mask |= (1U << 0);

    if (xTaskCreate(vTaskProcessing, "Proc", 2048, NULL, TASK_PROC_PRIO, NULL) == pdPASS)
        g_dbg_task_create_mask |= (1U << 1);
    else
        g_dbg_task_fail_mask |= (1U << 1);

    if (xTaskCreate(vTaskUI, "UI", 2048, NULL, TASK_UI_PRIO, NULL) == pdPASS)
        g_dbg_task_create_mask |= (1U << 2);
    else
        g_dbg_task_fail_mask |= (1U << 2);

    g_dbg_task_create_mask |= (1U << 3);
#endif

    g_dbg_boot_phase = 2;
    UI_UpdateDebug(0, 0, 0xA6, (uint8_t)g_dbg_task_fail_mask,
                   0, 0, 0xCE, 0xCE, 0xCE, 0, 0, 1,
                   g_dbg_task_create_mask, 0, 0);
    lv_timer_handler();

    xil_printf("Starting Scheduler...\r\n");
    g_dbg_boot_phase = 3;
    vTaskStartScheduler();

    g_dbg_boot_phase = 0xEE;
    UI_UpdateDebug(0, 0, 0xEE, (uint8_t)g_dbg_task_fail_mask,
                   0, 0, 0xEE, 0xEE, 0xEE, 0, 0, 1,
                   g_dbg_task_create_mask, 0, 0);
    lv_timer_handler();

    while (1);
    return 0;
}

void vTaskSensor(void *pvParameters) {
    (void)pvParameters;
    ppg_data_t samples[MAX_FIFO_BURST_SAMPLES];
    MAX30102_Config_t config = {
        .sample_rate = MAX30102_SR_100,
        .led_current = MAX30102_LED_CUR_12MA
    };
    uint8_t saturation_count = 0;
    uint8_t low_signal_count = 0;
    bool sensor_ready = false;
    bool irq_installed = false;
    TickType_t last_sample_tick = 0;
    TickType_t last_reg_tick = 0;
    TickType_t last_sps_tick = 0;
    uint32_t samples_this_sec = 0;
    MAX30102_DebugRegs_t dbg_regs;

    for (;;) {
        g_dbg_sensor_ticks++;

        if (!sensor_ready) {
            if (MAX30102_Init(&config) == XST_SUCCESS) {
                sensor_ready = true;
                g_sensor_online = true;
                last_sample_tick = xTaskGetTickCount();
#if USE_MAX30102_IRQ
                if (!irq_installed) {
                    (void)MAX30102_ReadInterruptStatus();
                    if (Setup_Interrupt_System(&IntcInstance, MAX30102_INT_ID) == XST_SUCCESS) {
                        irq_installed = true;
                        g_dbg_irq_enabled = 1U;
                    }
                    (void)xSemaphoreTake(xSensorDataReady, 0); /* discard stale edge */
                }
#endif
            } else {
                g_sensor_online = false;
                if (MAX30102_ReadDebugRegs(&dbg_regs) == XST_SUCCESS || dbg_regs.i2c_ok == 0U) {
                    update_debug_from_regs(&dbg_regs);
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

#if USE_MAX30102_IRQ
        if (xSemaphoreTake(xSensorDataReady, pdMS_TO_TICKS(500)) == pdTRUE) {
            g_dbg_int_status = MAX30102_ReadInterruptStatus();
        } else {
            /* Watchdog only: acquisition is interrupt-driven, but this prevents
             * a disconnected INT wire from making the monitor appear frozen.
             */
            g_dbg_irq_timeout++;
            g_dbg_int_status = MAX30102_ReadInterruptStatus();
        }
#else
        vTaskDelay(SENSOR_POLL_TICKS);
        g_dbg_int_status = MAX30102_ReadInterruptStatus();
#endif

        uint8_t n = 0;
        if (MAX30102_ReadSamples(samples, MAX_FIFO_BURST_SAMPLES, &n) == XST_SUCCESS) {
            if (n == 0U && (g_dbg_int_status & 0x40U) != 0U) {
                if (MAX30102_ReadFIFO(&samples[0].red, &samples[0].ir) == XST_SUCCESS) n = 1U;
            }

            for (uint8_t i = 0; i < n; ++i) {
                g_dbg_last_red = samples[i].red;
                g_dbg_last_ir = samples[i].ir;
                g_dbg_sensor_total++;
                if (samples[i].red > 258000U || samples[i].ir > 258000U) {
                    saturation_count++;
                    low_signal_count = 0U;
                } else if (samples[i].red < 5000U && samples[i].ir < 5000U) {
                    low_signal_count++;
                    if (saturation_count > 0U) saturation_count--;
                } else {
                    if (saturation_count > 0U) saturation_count--;
                    if (low_signal_count > 0U) low_signal_count--;
                }
                if (xQueueSend(xRawDataQueue, &samples[i], 0) != pdTRUE) {
                    ppg_data_t dropped;
                    (void)xQueueReceive(xRawDataQueue, &dropped, 0);
                    (void)xQueueSend(xRawDataQueue, &samples[i], 0);
                }
            }
            if (n > 0U) {
                last_sample_tick = xTaskGetTickCount();
                samples_this_sec += n;
            }
        } else {
            sensor_ready = false;
            g_sensor_online = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_reg_tick) > pdMS_TO_TICKS(250)) {
            /* Keep register debug fresh; this is cheap and visible without UART. */
            if (MAX30102_ReadDebugRegs(&dbg_regs) == XST_SUCCESS || dbg_regs.i2c_ok == 0U) {
                update_debug_from_regs(&dbg_regs);
            }
            last_reg_tick = now_tick;
        }
        if ((now_tick - last_sps_tick) > pdMS_TO_TICKS(1000)) {
            g_dbg_sensor_sps = samples_this_sec;
            samples_this_sec = 0;
            last_sps_tick = now_tick;
        }

        if (saturation_count > 20U) {
            /* Optical path saturated. Drop LED current in-place without sensor
             * reinitialization.  Reinitializing while a finger is present made
             * the display appear to freeze; LED PA registers are safe to update
             * while the MAX30102 is running.
             */
            if (config.led_current > MAX30102_LED_CUR_3MA) {
                if (config.led_current > MAX30102_LED_CUR_12MA) config.led_current = MAX30102_LED_CUR_12MA;
                else if (config.led_current > MAX30102_LED_CUR_6MA) config.led_current = MAX30102_LED_CUR_6MA;
                else config.led_current = MAX30102_LED_CUR_3MA;
                (void)MAX30102_SetLEDCurrent(config.led_current);
            }
            saturation_count = 0U;
            low_signal_count = 0U;
        }

        if (low_signal_count > 80U) {
            /* PPG too small. Increase LED current in-place; avoid FIFO/mode
             * reset so the waveform keeps streaming.
             */
            if (config.led_current < MAX30102_LED_CUR_50MA) {
                if (config.led_current < MAX30102_LED_CUR_6MA) config.led_current = MAX30102_LED_CUR_6MA;
                else if (config.led_current < MAX30102_LED_CUR_12MA) config.led_current = MAX30102_LED_CUR_12MA;
                else if (config.led_current < MAX30102_LED_CUR_25MA) config.led_current = MAX30102_LED_CUR_25MA;
                else config.led_current = MAX30102_LED_CUR_50MA;
                (void)MAX30102_SetLEDCurrent(config.led_current);
            }
            low_signal_count = 0U;
            saturation_count = 0U;
        }

        if ((xTaskGetTickCount() - last_sample_tick) > pdMS_TO_TICKS(SENSOR_OFFLINE_MS)) {
            if (!MAX30102_CheckStatus()) {
                sensor_ready = false;
                g_sensor_online = false;
            } else {
                sensor_ready = false; /* reinitialize FIFO/mode */
            }
        }
    }
}

void vTaskProcessing(void *pvParameters) {
    (void)pvParameters;
    ppg_data_t data;
    TickType_t last_proc_tick = xTaskGetTickCount();
    uint32_t proc_this_sec = 0;
    float red_dc_track = 0.0f;
    float ir_dc_track = 0.0f;

    static int32_t red_batch[FIR_BATCH_SAMPLES];
    static int32_t ir_batch[FIR_BATCH_SAMPLES];
    static int32_t red_hist[FIR_HISTORY_SAMPLES];
    static int32_t ir_hist[FIR_HISTORY_SAMPLES];
    static int32_t red_in[FIR_DMA_SAMPLES]  __attribute__((aligned(64)));
    static int32_t ir_in[FIR_DMA_SAMPLES]   __attribute__((aligned(64)));
    static int32_t red_out[FIR_DMA_SAMPLES] __attribute__((aligned(64)));
    static int32_t ir_out[FIR_DMA_SAMPLES]  __attribute__((aligned(64)));
    static ppg_data_t raw_batch[FIR_BATCH_SAMPLES];
    uint32_t batch_idx = 0;

    for (;;) {
        if (xQueueReceive(xRawDataQueue, &data, portMAX_DELAY) == pdTRUE) {
            proc_this_sec++;
            g_dbg_proc_total++;

            if (red_dc_track <= 1.0f) red_dc_track = (float)data.red;
            if (ir_dc_track  <= 1.0f) ir_dc_track  = (float)data.ir;
            red_dc_track = 0.99f * red_dc_track + 0.01f * (float)data.red;
            ir_dc_track  = 0.99f * ir_dc_track  + 0.01f * (float)data.ir;
            int32_t red_ac_sw = (int32_t)((float)data.red - red_dc_track);
            int32_t ir_ac_sw  = (int32_t)((float)data.ir  - ir_dc_track);

            raw_batch[batch_idx] = data;
            /* Strict FIR path: display and final HR/SpO2 AC are based on PL FIR
             * output only.  Feed DC-removed raw samples into the PPG band-pass
             * FIR so the filter works on the pulsatile component rather than
             * wasting dynamic range on the optical DC level.
             */
            red_batch[batch_idx] = red_ac_sw;
            ir_batch[batch_idx]  = ir_ac_sw;
            batch_idx++;

            if (batch_idx >= FIR_BATCH_SAMPLES) {
                /* Overlap-prefix the DMA packet with recent history.  The FIR
                 * Compiler is configured for packet framing/reset-on-vector, so
                 * sending history before new data greatly reduces visible packet
                 * boundary transients while keeping the displayed samples strictly
                 * post-PL-FIR.
                 */
                for (uint32_t i = 0; i < FIR_HISTORY_SAMPLES; ++i) {
                    red_in[i] = red_hist[i];
                    ir_in[i]  = ir_hist[i];
                }
                for (uint32_t i = 0; i < FIR_BATCH_SAMPLES; ++i) {
                    red_in[FIR_HISTORY_SAMPLES + i] = red_batch[i];
                    ir_in[FIR_HISTORY_SAMPLES + i]  = ir_batch[i];
                }

                if (DMA_FIR_FilterPair(red_in, red_out, ir_in, ir_out, FIR_DMA_SAMPLES) == XST_SUCCESS) {
                    g_dbg_fir_ok_total++;
                    for (uint32_t i = 0; i < FIR_BATCH_SAMPLES; ++i) {
                        fir_wave_sample_t wave;
                        uint32_t oi = FIR_HISTORY_SAMPLES + i;
                        wave.red_ac = red_out[oi];
                        wave.ir_ac  = ir_out[oi];

                        /* HR and SpO2 now use PL FIR output for AC. Raw samples
                         * are used only for DC in SpO2, which is physically
                         * required by the ratio-of-ratios method.
                         */
                        g_latest_spo2 = SPO2_CalculateFiltered(raw_batch[i].red, raw_batch[i].ir,
                                                               wave.red_ac, wave.ir_ac);
                        g_latest_hr   = HR_CalculateFiltered(wave.ir_ac, 100U);

                        /* Display decimation: MAX30102 runs at 100 Hz, monitor
                         * sweep displays at 50 Hz. HR/SpO2 still consume every
                         * FIR output sample above; only visualization is decimated.
                         */
                        if ((i & 1U) == 0U) {
                            if (xQueueSend(xWaveDataQueue, &wave, 0) != pdTRUE) {
                                fir_wave_sample_t dropped;
                                (void)xQueueReceive(xWaveDataQueue, &dropped, 0);
                                (void)xQueueSend(xWaveDataQueue, &wave, 0);
                            }
                        }
                    }
                } else {
                    /* Strict post-FIR requirement: do NOT fall back to raw
                     * waveform.  Count the error and leave the last trace on
                     * screen until FIR/DMA recovers.
                     */
                    g_dbg_fir_fail_total++;
                }
                /* Preserve the newest samples as prefix history for next packet. */
                for (uint32_t i = 0; i < FIR_HISTORY_SAMPLES; ++i) {
                    uint32_t src = FIR_BATCH_SAMPLES - FIR_HISTORY_SAMPLES + i;
                    red_hist[i] = red_batch[src];
                    ir_hist[i]  = ir_batch[src];
                }

                g_dbg_dma_ready_mask = DMA_GetReadyMask();
                g_dbg_dma_error_mask = DMA_GetErrorMask();
                batch_idx = 0;
            }

            if ((xTaskGetTickCount() - last_proc_tick) > pdMS_TO_TICKS(1000)) {
                g_dbg_proc_pps = proc_this_sec;
                proc_this_sec = 0;
                last_proc_tick = xTaskGetTickCount();
            }
        }
    }
}

void vTaskUI(void *pvParameters) {
    (void)pvParameters;
    fir_wave_sample_t wave;
    float render_red = 0.0f, render_ir = 0.0f;
    float target_red = 0.0f, target_ir = 0.0f;
    bool render_init = false;

    uint32_t last_wave_ms = 0;
    uint32_t last_metric_ms = 0;
    uint32_t last_debug_ms = 0;
    uint32_t last_status_ms = 0;
    uint32_t last_lvgl_ms = 0;

    for (;;) {
        g_dbg_ui_ticks++;
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;

        /* True streaming monitor policy:
         * - The sweep advances at a fixed 100 Hz (10 ms/point).
         * - Consume at most one FIR sample per sweep tick, preserving temporal
         *   order instead of draining a whole batch to the newest value.
         * - If the UI ever falls behind, drop old FIR samples down to a small
         *   backlog so latency stays bounded.
         * - If no new sample is available, keep sweeping with the last rendered
         *   value so the trace never visually freezes when acquisition pauses.
         */
        while (uxQueueMessagesWaiting(xWaveDataQueue) > 24U) {
            (void)xQueueReceive(xWaveDataQueue, &wave, 0);
        }

        if (last_wave_ms == 0U) last_wave_ms = now_ms;
        uint32_t steps = 0;
        while ((now_ms - last_wave_ms) >= 10U && steps < 3U) {
            if (xQueueReceive(xWaveDataQueue, &wave, 0) == pdTRUE) {
                target_red = (float)wave.red_ac;
                target_ir  = (float)wave.ir_ac;
                if (!render_init) {
                    render_red = target_red;
                    render_ir = target_ir;
                    render_init = true;
                }
            }
            if (render_init) {
                render_red = 0.25f * render_red + 0.75f * target_red;
                render_ir  = 0.25f * render_ir  + 0.75f * target_ir;
                UI_PushFilteredWaveform((int32_t)render_red, (int32_t)render_ir);
                g_dbg_ui_push_total++;
            }
            last_wave_ms += 10U;
            steps++;
        }

        if ((now_ms - last_metric_ms) >= 250U) {
            UI_UpdateMetrics(g_latest_spo2, g_latest_hr);
            last_metric_ms = now_ms;
        }
        if ((now_ms - last_status_ms) >= 500U) {
            UI_UpdateSensorStatus(g_sensor_online);
            last_status_ms = now_ms;
        }
        if ((now_ms - last_debug_ms) >= 1000U) {
            UI_UpdateDebug(g_dbg_sensor_sps, g_dbg_proc_pps, g_dbg_int_status,
                           g_dbg_fifo_count, g_dbg_last_red, g_dbg_last_ir,
                           g_dbg_part_id, g_dbg_mode_config, g_dbg_spo2_config,
                           g_dbg_fifo_wr, g_dbg_fifo_rd, g_dbg_i2c_ok,
                           g_dbg_task_create_mask, g_dbg_ui_ticks, g_dbg_ctrl_ticks);
            last_debug_ms = now_ms;
        }

        /* LVGL is now low-rate housekeeping only.  The fast waveform and large
         * numerics are framebuffer-rendered to avoid full-screen LVGL flushes. */
        if ((now_ms - last_lvgl_ms) >= 100U) {
            lv_timer_handler();
            last_lvgl_ms = now_ms;
        }
        vTaskDelay(1);
    }
}

void vTaskControl(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        g_dbg_ctrl_ticks++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vTaskMonitor(void *pvParameters)
{
    (void)pvParameters;

    MAX30102_Config_t config = {
        .sample_rate = MAX30102_SR_100,
        .led_current = MAX30102_LED_CUR_12MA
    };
    MAX30102_DebugRegs_t dbg_regs;
    ppg_data_t samples[MAX_FIFO_BURST_SAMPLES];

    float red_dc_track = 0.0f;
    float ir_dc_track = 0.0f;
    static int32_t red_batch[FIR_BATCH_SAMPLES];
    static int32_t ir_batch[FIR_BATCH_SAMPLES];
    static int32_t red_hist[FIR_HISTORY_SAMPLES];
    static int32_t ir_hist[FIR_HISTORY_SAMPLES];
    static int32_t red_in[FIR_DMA_SAMPLES]  __attribute__((aligned(64)));
    static int32_t ir_in[FIR_DMA_SAMPLES]   __attribute__((aligned(64)));
    static int32_t red_out[FIR_DMA_SAMPLES] __attribute__((aligned(64)));
    static int32_t ir_out[FIR_DMA_SAMPLES]  __attribute__((aligned(64)));
    static ppg_data_t raw_batch[FIR_BATCH_SAMPLES];
    uint32_t batch_idx = 0;
    uint32_t loop_count = 0;
    uint32_t samples_this_window = 0;
    uint8_t saturation_count = 0;
    uint8_t low_signal_count = 0;
    bool sensor_ready = false;

    for (;;) {
        g_dbg_ui_ticks++;
        g_dbg_sensor_ticks++;

        if (!sensor_ready) {
            if (MAX30102_Init(&config) == XST_SUCCESS) {
                sensor_ready = true;
                g_sensor_online = true;
                (void)MAX30102_ReadDebugRegs(&dbg_regs);
                update_debug_from_regs(&dbg_regs);
            } else {
                g_sensor_online = false;
                (void)MAX30102_ReadDebugRegs(&dbg_regs);
                update_debug_from_regs(&dbg_regs);
                UI_UpdateSensorStatus(false);
                UI_UpdateDebug(g_dbg_sensor_sps, g_dbg_proc_pps, g_dbg_int_status,
                               g_dbg_fifo_count, g_dbg_last_red, g_dbg_last_ir,
                               g_dbg_part_id, g_dbg_mode_config, g_dbg_spo2_config,
                               g_dbg_fifo_wr, g_dbg_fifo_rd, g_dbg_i2c_ok,
                               g_dbg_task_create_mask, g_dbg_ui_ticks, g_dbg_ctrl_ticks);
                lv_timer_handler();
                continue;
            }
        }

        g_dbg_int_status = MAX30102_ReadInterruptStatus();
        uint8_t n = 0;
        if (MAX30102_ReadSamples(samples, MAX_FIFO_BURST_SAMPLES, &n) != XST_SUCCESS) {
            sensor_ready = false;
            g_sensor_online = false;
            continue;
        }

        for (uint8_t si = 0; si < n; ++si) {
            ppg_data_t data = samples[si];
            g_dbg_last_red = data.red;
            g_dbg_last_ir = data.ir;
            g_dbg_sensor_total++;
            g_dbg_proc_total++;
            samples_this_window++;

            if (data.red > 258000U || data.ir > 258000U) {
                saturation_count++;
                low_signal_count = 0U;
            } else if (data.red < 5000U && data.ir < 5000U) {
                low_signal_count++;
                if (saturation_count > 0U) saturation_count--;
            } else {
                if (saturation_count > 0U) saturation_count--;
                if (low_signal_count > 0U) low_signal_count--;
            }

            if (red_dc_track <= 1.0f) red_dc_track = (float)data.red;
            if (ir_dc_track  <= 1.0f) ir_dc_track  = (float)data.ir;
            red_dc_track = 0.99f * red_dc_track + 0.01f * (float)data.red;
            ir_dc_track  = 0.99f * ir_dc_track  + 0.01f * (float)data.ir;
            int32_t red_ac_sw = (int32_t)((float)data.red - red_dc_track);
            int32_t ir_ac_sw  = (int32_t)((float)data.ir  - ir_dc_track);

            raw_batch[batch_idx] = data;
            red_batch[batch_idx] = red_ac_sw;
            ir_batch[batch_idx]  = ir_ac_sw;
            batch_idx++;

            if (batch_idx >= FIR_BATCH_SAMPLES) {
                for (uint32_t i = 0; i < FIR_HISTORY_SAMPLES; ++i) {
                    red_in[i] = red_hist[i];
                    ir_in[i]  = ir_hist[i];
                }
                for (uint32_t i = 0; i < FIR_BATCH_SAMPLES; ++i) {
                    red_in[FIR_HISTORY_SAMPLES + i] = red_batch[i];
                    ir_in[FIR_HISTORY_SAMPLES + i]  = ir_batch[i];
                }

                if (DMA_FIR_FilterPair(red_in, red_out, ir_in, ir_out, FIR_DMA_SAMPLES) == XST_SUCCESS) {
                    g_dbg_fir_ok_total++;
                    for (uint32_t i = 0; i < FIR_BATCH_SAMPLES; ++i) {
                        uint32_t oi = FIR_HISTORY_SAMPLES + i;
                        int32_t red_fir = red_out[oi];
                        int32_t ir_fir  = ir_out[oi];

                        g_latest_spo2 = SPO2_CalculateFiltered(raw_batch[i].red, raw_batch[i].ir,
                                                               red_fir, ir_fir);
                        g_latest_hr   = HR_CalculateFiltered(ir_fir, 100U);
                        UI_PushFilteredWaveform(red_fir, ir_fir);
                        g_dbg_ui_push_total++;
                    }
                } else {
                    g_dbg_fir_fail_total++;
                }

                if (FIR_BATCH_SAMPLES >= FIR_HISTORY_SAMPLES) {
                    for (uint32_t i = 0; i < FIR_HISTORY_SAMPLES; ++i) {
                        uint32_t src = FIR_BATCH_SAMPLES - FIR_HISTORY_SAMPLES + i;
                        red_hist[i] = red_batch[src];
                        ir_hist[i]  = ir_batch[src];
                    }
                } else {
                    for (uint32_t i = 0; i < (FIR_HISTORY_SAMPLES - FIR_BATCH_SAMPLES); ++i) {
                        red_hist[i] = red_hist[i + FIR_BATCH_SAMPLES];
                        ir_hist[i]  = ir_hist[i + FIR_BATCH_SAMPLES];
                    }
                    for (uint32_t i = 0; i < FIR_BATCH_SAMPLES; ++i) {
                        red_hist[FIR_HISTORY_SAMPLES - FIR_BATCH_SAMPLES + i] = red_batch[i];
                        ir_hist[FIR_HISTORY_SAMPLES - FIR_BATCH_SAMPLES + i]  = ir_batch[i];
                    }
                }
                batch_idx = 0;
                g_dbg_dma_ready_mask = DMA_GetReadyMask();
                g_dbg_dma_error_mask = DMA_GetErrorMask();
            }
        }

        if (saturation_count > 20U) {
            if (config.led_current > MAX30102_LED_CUR_3MA) {
                if (config.led_current > MAX30102_LED_CUR_12MA) config.led_current = MAX30102_LED_CUR_12MA;
                else if (config.led_current > MAX30102_LED_CUR_6MA) config.led_current = MAX30102_LED_CUR_6MA;
                else config.led_current = MAX30102_LED_CUR_3MA;
                (void)MAX30102_SetLEDCurrent(config.led_current);
            }
            saturation_count = 0U;
        }
        if (low_signal_count > 80U) {
            if (config.led_current < MAX30102_LED_CUR_50MA) {
                if (config.led_current < MAX30102_LED_CUR_6MA) config.led_current = MAX30102_LED_CUR_6MA;
                else if (config.led_current < MAX30102_LED_CUR_12MA) config.led_current = MAX30102_LED_CUR_12MA;
                else if (config.led_current < MAX30102_LED_CUR_25MA) config.led_current = MAX30102_LED_CUR_25MA;
                else config.led_current = MAX30102_LED_CUR_50MA;
                (void)MAX30102_SetLEDCurrent(config.led_current);
            }
            low_signal_count = 0U;
        }

        loop_count++;
        if ((loop_count & 0x0FU) == 0U) {
            (void)MAX30102_ReadDebugRegs(&dbg_regs);
            update_debug_from_regs(&dbg_regs);
            UI_UpdateMetrics(g_latest_spo2, g_latest_hr);
            UI_UpdateSensorStatus(g_sensor_online);
            UI_UpdateDebug(g_dbg_sensor_sps, g_dbg_proc_pps, g_dbg_int_status,
                           g_dbg_fifo_count, g_dbg_last_red, g_dbg_last_ir,
                           g_dbg_part_id, g_dbg_mode_config, g_dbg_spo2_config,
                           g_dbg_fifo_wr, g_dbg_fifo_rd, g_dbg_i2c_ok,
                           g_dbg_task_create_mask, g_dbg_ui_ticks, g_dbg_ctrl_ticks);
            lv_timer_handler();
        }
        if ((loop_count & 0x7FU) == 0U) {
            g_dbg_sensor_sps = samples_this_window;
            g_dbg_proc_pps = samples_this_window;
            samples_this_window = 0;
        }

        taskYIELD();
    }
}
