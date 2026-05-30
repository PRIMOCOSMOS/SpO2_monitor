#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "xil_printf.h"
#include "xparameters.h"

/* Project Includes */
#include "MAX30102/max30102.h"
#include "SPO2/spo2_calc.h"
#include "HR_algor/hr_calc.h"
#include "display_ctrl/display_ctrl.h"
#include "PWM/pwm_ctrl.h"
#include "DMA/dma_ctrl.h"
#include "IIC_PL/iic_pl_ctrl.h"
#include "third_party/lvgl/lvgl.h"

/* Task Priorities */
#define TASK_SENSOR_PRIO    ( tskIDLE_PRIORITY + 5 )
#define TASK_PROC_PRIO      ( tskIDLE_PRIORITY + 4 )
#define TASK_UI_PRIO        ( tskIDLE_PRIORITY + 3 )
#define TASK_CTRL_PRIO      ( tskIDLE_PRIORITY + 2 )

/* Queues & Semaphores */
QueueHandle_t xRawDataQueue;
SemaphoreHandle_t xGuiSemaphore;

/* Task Prototypes */
void vTaskSensor(void *pvParameters);
void vTaskProcessing(void *pvParameters);
void vTaskUI(void *pvParameters);
void vTaskControl(void *pvParameters);

int main() {
    xil_printf("--- SpO2 Monitor Jumpstart (Zynq UltraScale+) ---\r\n");

    /* Initialize Hardware Components */
    IIC_PL_Init();
    DMA_Init();
    PWM_Init();
    Display_Init(); // VDMA + VTC
    
    /* Initialize UI Framework */
    lv_init();
    UI_Init(); // Layout setup

    /* Create Queues */
    xRawDataQueue = xQueueCreate(10, sizeof(ppg_data_t));
    xGuiSemaphore = xSemaphoreCreateMutex();

    /* Create Tasks */
    xTaskCreate(vTaskSensor, "SensorTask", 2048, NULL, TASK_SENSOR_PRIO, NULL);
    xTaskCreate(vTaskProcessing, "ProcTask", 2048, NULL, TASK_PROC_PRIO, NULL);
    xTaskCreate(vTaskUI, "UITask", 4096, NULL, TASK_UI_PRIO, NULL);
    xTaskCreate(vTaskControl, "CtrlTask", 1024, NULL, TASK_CTRL_PRIO, NULL);

    /* Start Scheduler */
    vTaskStartScheduler();

    while(1);
    return 0;
}

/**
 * @brief High-frequency sensor sampling task
 */
void vTaskSensor(void *pvParameters) {
    ppg_data_t raw_sample;
    MAX30102_Config_t config = {
        .sample_rate = MAX30102_SR_400,
        .led_current = MAX30102_LED_CUR_50MA
    };

    if (MAX30102_Init(&config) != XST_SUCCESS) {
        xil_printf("Error: MAX30102 not detected!\r\n");
    }

    for (;;) {
        // Read Red and IR data
        if (MAX30102_ReadFIFO(&raw_sample.red, &raw_sample.ir) == XST_SUCCESS) {
            // Send to processing task via queue
            xQueueSend(xRawDataQueue, &raw_sample, portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(5)); // ~200Hz
    }
}

/**
 * @brief Algorithm processing task
 */
void vTaskProcessing(void *pvParameters) {
    ppg_data_t data;
    float spo2_val = 0;
    int hr_val = 0;

    for (;;) {
        if (xQueueReceive(xRawDataQueue, &data, portMAX_DELAY) == pdTRUE) {
            // Apply algorithms
            spo2_val = SPO2_Calculate(data.red, data.ir);
            hr_val = HR_Calculate(data.ir);

            // Update Global UI State (Protected by Mutex)
            if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
                UI_UpdateMetrics(spo2_val, hr_val);
                UI_PushWaveform(data.red, data.ir);
                xSemaphoreGive(xGuiSemaphore);
            }
        }
    }
}

/**
 * @brief LVGL Refresh Task
 */
void vTaskUI(void *pvParameters) {
    for (;;) {
        if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * @brief Maintenance and System Check
 */
void vTaskControl(void *pvParameters) {
    uint8_t brightness = 80;
    for (;;) {
        // Periodic sensor status check
        bool online = MAX30102_CheckStatus();
        UI_UpdateSensorStatus(online);

        // Brightness control (Example)
        PWM_SetBrightness(brightness);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
