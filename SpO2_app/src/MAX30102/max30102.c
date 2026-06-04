#include "max30102.h"
#include "../IIC_PL/iic_pl_ctrl.h"
#include "xil_printf.h"
#include "FreeRTOS.h"
#include "task.h"

#define MAX30102_ADDR       0x57
#define REG_INTR_STATUS_1   0x00
#define REG_INTR_STATUS_2   0x01
#define REG_INTR_ENABLE_1   0x02
#define REG_INTR_ENABLE_2   0x03
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C   /* Red LED in SpO2 mode */
#define REG_LED2_PA         0x0D   /* IR LED in SpO2 mode  */
#define REG_PART_ID         0xFF

static void max30102_delay_ms(uint32_t ms)
{
    /* Use a bounded busy wait instead of vTaskDelay.  The application can run
     * in a single always-ready monitor task; relying on the RTOS tick here can
     * deadlock bring-up if the tick/timer is not yet healthy.
     */
    volatile uint32_t loops = ms * 100000U;
    while (loops--) { __asm__ volatile("nop"); }
}

static XStatus write_checked(uint8_t reg, uint8_t val)
{
    XStatus st = IIC_PL_WriteReg(MAX30102_ADDR, reg, val);
    if (st != XST_SUCCESS) {
        xil_printf("[MAX30102] I2C write failed: reg=0x%02x\r\n", reg);
    }
    return st;
}

XStatus MAX30102_Init(MAX30102_Config_t *config) {
    if (config == NULL) return XST_FAILURE;

    if (!MAX30102_CheckStatus()) {
        xil_printf("[MAX30102] Part ID mismatch or I2C read failed. Expected 0x15.\r\n");
        return XST_FAILURE;
    }

    /* Soft reset and wait until the device is ready. */
    if (write_checked(REG_MODE_CONFIG, 0x40) != XST_SUCCESS) return XST_FAILURE;
    max30102_delay_ms(100);

    uint8_t mode = 0x40;
    for (int i = 0; i < 20; ++i) {
        if (IIC_PL_ReadReg(MAX30102_ADDR, REG_MODE_CONFIG, &mode) == XST_SUCCESS &&
            ((mode & 0x40U) == 0U)) {
            break;
        }
        max30102_delay_ms(10);
    }
    if (mode & 0x40U) {
        xil_printf("[MAX30102] Reset timeout.\r\n");
        return XST_FAILURE;
    }

    /* Disable interrupts while configuring, then clear pending status. */
    if (write_checked(REG_INTR_ENABLE_1, 0x00) != XST_SUCCESS) return XST_FAILURE;
    if (write_checked(REG_INTR_ENABLE_2, 0x00) != XST_SUCCESS) return XST_FAILURE;
    (void)MAX30102_ReadInterruptStatus();
    uint8_t dummy;
    (void)IIC_PL_ReadReg(MAX30102_ADDR, REG_INTR_STATUS_2, &dummy);

    /* Reset FIFO pointers and overflow counter. */
    if (write_checked(REG_FIFO_WR_PTR, 0x00) != XST_SUCCESS) return XST_FAILURE;
    if (write_checked(REG_OVF_COUNTER, 0x00) != XST_SUCCESS) return XST_FAILURE;
    if (write_checked(REG_FIFO_RD_PTR, 0x00) != XST_SUCCESS) return XST_FAILURE;

    /* FIFO: sample average = 1, rollover enabled, almost-full threshold = 17 samples.
     * Use averaging=1 while bringing up the data path so FIFO pointers move at
     * the selected sample rate and waveform latency is minimal.
     */
    if (write_checked(REG_FIFO_CONFIG, 0x1F) != XST_SUCCESS) return XST_FAILURE;

    /* SpO2 config: ADC range = 16384 nA (bits 6:5 = 3), selected sample
     * rate, 411 us pulse width / 18-bit ADC.  The previous 0x00 ADC range
     * saturated at 0x3FFFF when a finger was placed on the sensor.
     */
    if (write_checked(REG_SPO2_CONFIG, 0x60U | ((uint8_t)config->sample_rate << 2) | 0x03U) != XST_SUCCESS) return XST_FAILURE;

    /* LED pulse amplitudes. LED1=Red, LED2=IR. */
    if (write_checked(REG_LED1_PA, (uint8_t)config->led_current) != XST_SUCCESS) return XST_FAILURE;
    if (write_checked(REG_LED2_PA, (uint8_t)config->led_current) != XST_SUCCESS) return XST_FAILURE;

    /* SpO2 mode: Red + IR slots. */
    if (write_checked(REG_MODE_CONFIG, 0x03) != XST_SUCCESS) return XST_FAILURE;

    /* Interrupt-driven acquisition: enable PPG_RDY + A_FULL.  PPG_RDY gives a
     * reliable data-ready interrupt on this PL/GIC path; the sensor task still
     * drains FIFO in bursts when multiple samples are available.
     */
    if (write_checked(REG_INTR_ENABLE_1, 0xC0) != XST_SUCCESS) return XST_FAILURE;
    (void)MAX30102_ReadInterruptStatus();

    return XST_SUCCESS;
}

XStatus MAX30102_GetFifoSampleCount(uint8_t *count)
{
    uint8_t wr = 0, rd = 0, ovf = 0;
    if (count == NULL) return XST_FAILURE;
    if (IIC_PL_ReadReg(MAX30102_ADDR, REG_FIFO_WR_PTR, &wr) != XST_SUCCESS) return XST_FAILURE;
    if (IIC_PL_ReadReg(MAX30102_ADDR, REG_FIFO_RD_PTR, &rd) != XST_SUCCESS) return XST_FAILURE;
    (void)IIC_PL_ReadReg(MAX30102_ADDR, REG_OVF_COUNTER, &ovf);

    uint8_t n = (uint8_t)((wr - rd) & 0x1F); /* 5-bit circular FIFO pointers */
    if (n == 0U && ovf != 0U) {
        /* When FIFO overflows, WR/RD can become equal although unread samples
         * exist/old data was overwritten.  Drain a full FIFO to resynchronise.
         */
        n = MAX30102_FIFO_DEPTH;
    }

    *count = n;
    return XST_SUCCESS;
}

XStatus MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir) {
    uint8_t buffer[6];
    if (red == NULL || ir == NULL) return XST_FAILURE;

    if (IIC_PL_ReadBuf(MAX30102_ADDR, REG_FIFO_DATA, buffer, sizeof(buffer)) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    *red = (((uint32_t)buffer[0] << 16) | ((uint32_t)buffer[1] << 8) | (uint32_t)buffer[2]) & 0x3FFFFU;
    *ir  = (((uint32_t)buffer[3] << 16) | ((uint32_t)buffer[4] << 8) | (uint32_t)buffer[5]) & 0x3FFFFU;
    return XST_SUCCESS;
}

XStatus MAX30102_ReadSamples(ppg_data_t *samples, uint8_t max_samples, uint8_t *samples_read)
{
    uint8_t count = 0;
    uint8_t n;

    if (samples_read != NULL) *samples_read = 0;
    if (samples == NULL || max_samples == 0U) return XST_FAILURE;

    if (MAX30102_GetFifoSampleCount(&count) != XST_SUCCESS) return XST_FAILURE;
    if (count == 0U) return XST_SUCCESS;

    if (count > MAX30102_FIFO_DEPTH) count = MAX30102_FIFO_DEPTH;
    n = (count > max_samples) ? max_samples : count;

    for (uint8_t i = 0; i < n; ++i) {
        if (MAX30102_ReadFIFO(&samples[i].red, &samples[i].ir) != XST_SUCCESS) {
            return XST_FAILURE;
        }
    }

    if (samples_read != NULL) *samples_read = n;
    return XST_SUCCESS;
}

uint8_t MAX30102_ReadInterruptStatus(void) {
    uint8_t status = 0;
    (void)IIC_PL_ReadReg(MAX30102_ADDR, REG_INTR_STATUS_1, &status);
    return status;
}

XStatus MAX30102_SetLEDCurrent(MAX30102_LED_CUR_t led_current)
{
    if (write_checked(REG_LED1_PA, (uint8_t)led_current) != XST_SUCCESS) return XST_FAILURE;
    if (write_checked(REG_LED2_PA, (uint8_t)led_current) != XST_SUCCESS) return XST_FAILURE;
    return XST_SUCCESS;
}

XStatus MAX30102_ReadDebugRegs(MAX30102_DebugRegs_t *dbg)
{
    XStatus st = XST_SUCCESS;
    if (dbg == NULL) return XST_FAILURE;

    dbg->i2c_ok = 1U;
#define READ_DBG(reg, field) \
    do { \
        if (IIC_PL_ReadReg(MAX30102_ADDR, (reg), &dbg->field) != XST_SUCCESS) { \
            dbg->field = 0xEEU; \
            dbg->i2c_ok = 0U; \
            st = XST_FAILURE; \
        } \
    } while (0)

    READ_DBG(REG_INTR_STATUS_1, int_status_1);   /* read-to-clear */
    READ_DBG(REG_INTR_STATUS_2, int_status_2);   /* read-to-clear */
    READ_DBG(REG_INTR_ENABLE_1, int_enable_1);
    READ_DBG(REG_INTR_ENABLE_2, int_enable_2);
    READ_DBG(REG_FIFO_WR_PTR, fifo_wr_ptr);
    READ_DBG(REG_OVF_COUNTER, fifo_ovf_counter);
    READ_DBG(REG_FIFO_RD_PTR, fifo_rd_ptr);
    READ_DBG(REG_FIFO_CONFIG, fifo_config);
    READ_DBG(REG_MODE_CONFIG, mode_config);
    READ_DBG(REG_SPO2_CONFIG, spo2_config);
    READ_DBG(REG_LED1_PA, led1_pa);
    READ_DBG(REG_LED2_PA, led2_pa);
    READ_DBG(0xFE, revision_id);
    READ_DBG(REG_PART_ID, part_id);
#undef READ_DBG

    dbg->fifo_count = (uint8_t)((dbg->fifo_wr_ptr - dbg->fifo_rd_ptr) & 0x1FU);
    if (dbg->fifo_count == 0U && dbg->fifo_ovf_counter != 0U && dbg->fifo_ovf_counter != 0xEEU) {
        dbg->fifo_count = MAX30102_FIFO_DEPTH;
    }
    return st;
}

bool MAX30102_CheckStatus(void) {
    uint8_t id = 0;
    if (IIC_PL_ReadReg(MAX30102_ADDR, REG_PART_ID, &id) == XST_SUCCESS) {
        return (id == MAX30102_EXPECTED_PART_ID); /* MAX30102 PART_ID must be 0x15. */
    }
    return false;
}
