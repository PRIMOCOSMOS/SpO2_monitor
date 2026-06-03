#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xaxivdma.h"
#include "xscugic.h"
#include "xil_cache.h"

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

/* Global Handles */
QueueHandle_t xRawDataQueue = NULL;
SemaphoreHandle_t xGuiSemaphore = NULL;
SemaphoreHandle_t xSensorDataReady = NULL;

/* Sensor online flag (maintained by vTaskSensor, read by vTaskControl) */
volatile bool g_sensor_online = false;

/* Hardware Instances */
static XScuGic IntcInstance;
static XAxiVdma VdmaInstance;
DisplayCtrl gDispCtrl;   /* non-static: ui_manager.c calls DisplayChangeFrame */

/* Hardware Settings - Derived from pl.dtsi */
#define MAX30102_INT_ID     136
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

/* Task Prototypes */
void vTaskSensor(void *pvParameters);
void vTaskProcessing(void *pvParameters);
void vTaskUI(void *pvParameters);
void vTaskControl(void *pvParameters);

int main() {
    XStatus Status;
    xil_printf("\r\n--- SpO2 Monitor Start ---\r\n");

    /* 1. Framebuffer pointer setup (must be first) */
    for (int i = 0; i < DISPLAY_NUM_FRAMES; i++) {
        pFrames[i] = frameBuf[i];
        memset(pFrames[i], 0, DISPLAY_HEIGHT * STRIDE); /* clear to black */
    }

    /* 2. Basic peripheral init */
    IIC_PL_Init();
    DMA_Init();

    /* 3. Backlight PWM (must happen before display, no vTaskDelay here) */
    set_pwm_frequency(PWM_BASEADDR, 100000000, 1000.0f);
    set_pwm_duty(PWM_BASEADDR, 0.80f);

    /* 4. VDMA driver instance init (Digilent display_ctrl does NOT do this) */
    XAxiVdma_Config *vdmaConfig = XAxiVdma_LookupConfig(VDMA_DEVICE_ID);
    if (vdmaConfig == NULL) {
        xil_printf("CRITICAL: XAxiVdma_LookupConfig failed!\r\n");
        while (1); /* halt */
    }
    Status = XAxiVdma_CfgInitialize(&VdmaInstance, vdmaConfig, vdmaConfig->BaseAddress);
    if (Status != XST_SUCCESS) {
        xil_printf("CRITICAL: XAxiVdma_CfgInitialize failed (%d)!\r\n", Status);
        while (1); /* halt */
    }
    xil_printf("[VDMA] Instance initialized OK.\r\n");

    /* 5. Display pipeline init + start */
    Status = DisplayInitialize(&gDispCtrl, &VdmaInstance, VTC_BASEADDR, DYNCLK_BASEADDR, pFrames, STRIDE);
    if (Status != XST_SUCCESS) {
        xil_printf("CRITICAL: DisplayInitialize failed (%d)!\r\n", Status);
    }
    DisplaySetMode(&gDispCtrl, &VMODE_800x480);
    Status = DisplayStart(&gDispCtrl);
    if (Status != XST_SUCCESS) {
        xil_printf("CRITICAL: DisplayStart failed (%d) — check VDMA stride/address/clock!\r\n", Status);
    } else {
        xil_printf("[Display] Pipeline started OK.\r\n");
    }

    /* 6. LVGL init (before scheduler) */
    lv_init();
    UI_Init();

    /* 7. FreeRTOS sync primitives */
    xRawDataQueue = xQueueCreate(64, sizeof(ppg_data_t));
    xGuiSemaphore = xSemaphoreCreateMutex();
    xSensorDataReady = xSemaphoreCreateBinary();

    /* 8. Interrupt controller + MAX30102 ISR hook */
    Setup_Interrupt_System(&IntcInstance, MAX30102_INT_ID);

    /* 9. Task creation */
    xTaskCreate(vTaskSensor,    "Sensor", 2048, NULL, TASK_SENSOR_PRIO, NULL);
    xTaskCreate(vTaskProcessing, "Proc",   2048, NULL, TASK_PROC_PRIO,   NULL);
    xTaskCreate(vTaskUI,        "UI",     4096, NULL, TASK_UI_PRIO,     NULL);
    xTaskCreate(vTaskControl,   "Ctrl",   1024, NULL, TASK_CTRL_PRIO,   NULL);

    xil_printf("Starting Scheduler...\r\n");
    vTaskStartScheduler();

    while (1);
    return 0;
}

void vTaskSensor(void *pvParameters) {
    (void)pvParameters;
    ppg_data_t sample;
    MAX30102_Config_t config = {
        .sample_rate = MAX30102_SR_400,
        .led_current = MAX30102_LED_CUR_50MA
    };
    bool sensor_ready = false;

    for (;;) {
        /* ---------- INIT / RETRY state ---------- */
        if (!sensor_ready) {
            if (MAX30102_Init(&config) == XST_SUCCESS) {
                xil_printf("[Sensor] MAX30102 detected! Entering normal acquisition.\r\n");
                sensor_ready = true;
                g_sensor_online = true;
            } else {
                xil_printf("[Sensor] Not detected. Retry in 2 s...\r\n");
                g_sensor_online = false;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;   /* retry immediately */
            }
        }

        /* ---------- ACQUISITION state ---------- */
        /* Block on ISR semaphore; if no interrupt for 3 s, assume unplugged */
        if (xSemaphoreTake(xSensorDataReady, pdMS_TO_TICKS(3000)) == pdTRUE) {
            while (MAX30102_ReadFIFO(&sample.red, &sample.ir) == XST_SUCCESS) {
                xQueueSend(xRawDataQueue, &sample, 0);
            }
        } else {
            xil_printf("[Sensor] Timeout (3 s). Lost connection -> return to detection.\r\n");
            sensor_ready = false;
            g_sensor_online = false;
        }
    }
}

void vTaskProcessing(void *pvParameters) {
    (void)pvParameters;
    ppg_data_t data;
    float spo2 = 0;
    int hr = 0;
    for (;;) {
        if (xQueueReceive(xRawDataQueue, &data, portMAX_DELAY) == pdTRUE) {
            spo2 = SPO2_Calculate(data.red, data.ir);
            hr   = HR_Calculate(data.ir);
            if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
                UI_UpdateMetrics(spo2, hr);
                UI_PushWaveform(data.red, data.ir);
                xSemaphoreGive(xGuiSemaphore);
            }
        }
    }
}

void vTaskUI(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void vTaskControl(void *pvParameters) {
    (void)pvParameters;
    bool last_online = false;

    for (;;) {
        bool online = g_sensor_online;
        if (online != last_online) {
            xil_printf("[Control] Sensor status change: %s\r\n", online ? "ONLINE" : "OFFLINE");
            last_online = online;
        }
        if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
            UI_UpdateSensorStatus(online);
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
