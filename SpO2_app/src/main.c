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

/* Hardware Instances */
static XScuGic IntcInstance;
static XAxiVdma VdmaInstance;
static DisplayCtrl DispCtrl;

/* Hardware Settings - Based on pl.dtsi */
#define MAX30102_INT_ID     136 
#define PWM_BASEADDR        XPAR_AX_PWM_0_BASEADDR
#define DYNCLK_BASEADDR     XPAR_AXI_DYNCLK_0_BASEADDR
#define VTC_BASEADDR        XPAR_V_TC_0_BASEADDR

/* Framebuffers for 800x480 (WVGA) */
#define DISPLAY_WIDTH  800
#define DISPLAY_HEIGHT 480
#define STRIDE         (DISPLAY_WIDTH * 4) 
uint8_t frameBuf[DISPLAY_NUM_FRAMES][DISPLAY_HEIGHT * STRIDE] __attribute__((aligned(256)));
uint8_t *pFrames[DISPLAY_NUM_FRAMES];

/* Task Prototypes */
void vTaskSensor(void *pvParameters);
void vTaskProcessing(void *pvParameters);
void vTaskUI(void *pvParameters);
void vTaskControl(void *pvParameters);

int main() {
    XStatus Status;
    xil_printf("--- SpO2 Monitor Start ---\r\n");

    /* 1. 初始化显存指针 (必须最先执行) */
    for (int i = 0; i < DISPLAY_NUM_FRAMES; i++) {
        pFrames[i] = frameBuf[i];
        memset(pFrames[i], 0, DISPLAY_HEIGHT * STRIDE); // 预清空显存为黑色
    }

    /* 2. 基础硬件初始化 */
    IIC_PL_Init();
    DMA_Init();
    
    /* 3. 背光与时钟 (注意：这里不要使用 vTaskDelay) */
    set_pwm_frequency(PWM_BASEADDR, 100000000, 1000.0f);
    set_pwm_duty(PWM_BASEADDR, 80.0f); 

    /* 4. 视频流初始化 */
    DisplayInitialize(&DispCtrl, &VdmaInstance, VTC_BASEADDR, DYNCLK_BASEADDR, pFrames, STRIDE);
    DisplaySetMode(&DispCtrl, &VMODE_800x480);
    Status = DisplayStart(&DispCtrl);
    if (Status != XST_SUCCESS) {
        xil_printf("CRITICAL: Display Pipeline failed to lock clock!\r\n");
    }

    /* 5. LVGL 初始化 (在启动 Scheduler 之前) */
    lv_init();
    UI_Init(); 

    /* 6. 创建同步对象 */
    xRawDataQueue = xQueueCreate(64, sizeof(ppg_data_t));
    xGuiSemaphore = xSemaphoreCreateMutex();
    xSensorDataReady = xSemaphoreCreateBinary();

    /* 7. 中断挂载 */
    Setup_Interrupt_System(&IntcInstance, MAX30102_INT_ID);

    /* 8. 创建任务 */
    xTaskCreate(vTaskSensor, "Sensor", 2048, NULL, TASK_SENSOR_PRIO, NULL);
    xTaskCreate(vTaskProcessing, "Proc", 2048, NULL, TASK_PROC_PRIO, NULL);
    xTaskCreate(vTaskUI, "UI", 4096, NULL, TASK_UI_PRIO, NULL);
    xTaskCreate(vTaskControl, "Ctrl", 1024, NULL, TASK_CTRL_PRIO, NULL);

    xil_printf("Starting Scheduler...\r\n");
    vTaskStartScheduler();

    while(1);
    return 0;
}

void vTaskSensor(void *pvParameters) {
    (void)pvParameters;
    ppg_data_t sample;
    MAX30102_Config_t config = { .sample_rate = MAX30102_SR_400, .led_current = MAX30102_LED_CUR_50MA };

    if (MAX30102_Init(&config) != XST_SUCCESS) {
        xil_printf("Sensor Init Failed!\r\n");
    }

    for (;;) {
        if (xSemaphoreTake(xSensorDataReady, portMAX_DELAY) == pdTRUE) {
            while (MAX30102_ReadFIFO(&sample.red, &sample.ir) == XST_SUCCESS) {
                xQueueSend(xRawDataQueue, &sample, 0);
            }
        }
    }
}

void vTaskProcessing(void *pvParameters) {
    (void)pvParameters;
    ppg_data_t data;
    float spo2 = 0; int hr = 0;
    for (;;) {
        if (xQueueReceive(xRawDataQueue, &data, portMAX_DELAY) == pdTRUE) {
            spo2 = SPO2_Calculate(data.red, data.ir);
            hr = HR_Calculate(data.ir);
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
    for (;;) {
        bool online = MAX30102_CheckStatus();
        if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
            UI_UpdateSensorStatus(online);
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
