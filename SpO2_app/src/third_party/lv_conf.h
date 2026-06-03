/**
 * @file lv_conf.h
 * Configuration file for v8.x or v9.x
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* clang-format off */

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Color settings */
#define LV_COLOR_DEPTH 32
#define LV_COLOR_SCREEN_TRANSP 0

/* Memory settings */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (128U * 1024U)
#define LV_MEM_ADR 0

/* HAL Settings */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "FreeRTOS.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (xTaskGetTickCount())

/* Feature usage */
#define LV_USE_CHART 1
#define LV_USE_LABEL 1
#define LV_USE_CANVAS 1

/* Font usage */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#endif /*LV_CONF_H*/
