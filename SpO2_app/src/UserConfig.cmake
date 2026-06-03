# Copyright (C) 2023-2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
enable_language(C ASM CXX)

### USER SETTINGS START ###
set(USER_COMPILE_DEFINITIONS "")
set(USER_UNDEFINED_SYMBOLS "__clang__")

# 1. 包含所有必要的头文件目录
set(USER_INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/DMA"
    "${CMAKE_CURRENT_SOURCE_DIR}/FIR"
    "${CMAKE_CURRENT_SOURCE_DIR}/HR_algor"
    "${CMAKE_CURRENT_SOURCE_DIR}/IIC_PL"
    "${CMAKE_CURRENT_SOURCE_DIR}/MAX30102"
    "${CMAKE_CURRENT_SOURCE_DIR}/SPO2"
    "${CMAKE_CURRENT_SOURCE_DIR}/interrupt"
    "${CMAKE_CURRENT_SOURCE_DIR}/PWM"
    "${CMAKE_CURRENT_SOURCE_DIR}/display_ctrl"
    "${CMAKE_CURRENT_SOURCE_DIR}/dynclk"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl"
)

# 2. 自动搜寻 LVGL 库的所有源代码 (这是解决 Undefined Reference 的关键)
file(GLOB_RECURSE LVGL_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl/src/*.c"
)

# 3. 汇总所有要编译的源文件
set(USER_COMPILE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/DMA/dma_ctrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/HR_algor/hr_calc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/IIC_PL/iic_pl_ctrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/MAX30102/max30102.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/SPO2/spo2_calc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/SPO2/ui_manager.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/interrupt/int_controller.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/PWM/ax_pwm.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/display_ctrl/display_ctrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dynclk/dynclk.c"
    ${LVGL_SOURCES} 
)

# -----------------------------------------
set(USER_COMPILE_WARNINGS_ALL -Wall)
set(USER_COMPILE_WARNINGS_EXTRA -Wextra)
set(USER_COMPILE_OPTIMIZATION_LEVEL -O2) # 建议开启优化，否则 LVGL 会很慢
set(USER_COMPILE_DEBUG_LEVEL -g3)
set(USER_COMPILE_ANSI )
set(USER_COMPILE_RELAXATION "-Wl,--no-relax")
set(USER_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/lscript.ld")

### END OF USER SETTINGS SECTION ###

set(USER_COMPILE_OPTIONS
 " ${USER_COMPILE_WARNINGS_ALL}"
 " ${USER_COMPILE_WARNINGS_EXTRA}"
 " ${USER_COMPILE_OPTIMIZATION_LEVEL}"
 " ${USER_COMPILE_DEBUG_LEVEL}"
 " ${USER_COMPILE_OTHER_FLAGS}"
)
foreach(entry ${USER_UNDEFINED_SYMBOLS})
 list(APPEND USER_COMPILE_OPTIONS " -U${entry}")
endforeach()

if(USER_LINK_DIRECTORIES)
 string(REPLACE ";" " -L" _formatted_dirs "${USER_LINK_DIRECTORIES}")
 set(USER_LINK_DIRECTORIES "${_formatted_dirs}")
endif()

set(USER_LINK_OPTIONS
 " ${USER_LINKER_NO_START_FILES}"
 " ${USER_LINKER_NO_DEFAULT_LIBS}"
 " ${USER_LINKER_NO_STDLIB}"
 " ${USER_LINK_OTHER_FLAGS}"
)
