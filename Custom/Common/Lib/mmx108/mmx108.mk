#
# Copyright 2022-2024 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0
#

MMX108_ROOT_PATH = ../Custom/Common/Lib/mmx108

C_SOURCES += $(wildcard $(MMX108_ROOT_PATH)/mm_shims/*.c)
C_SOURCES += $(wildcard $(MMX108_ROOT_PATH)/mmipal/lwip/*.c)
C_SOURCES += $(wildcard $(MMX108_ROOT_PATH)/mmpktmem/*.c)
C_SOURCES += $(wildcard $(MMX108_ROOT_PATH)/mmregdb/*.c)
C_SOURCES += $(wildcard $(MMX108_ROOT_PATH)/mmutils/*.c)

C_SOURCES += $(MMX108_ROOT_PATH)/halow_example.c

C_INCLUDES += -I$(MMX108_ROOT_PATH)
C_INCLUDES += -I$(MMX108_ROOT_PATH)/morselib/include
C_INCLUDES += -I$(MMX108_ROOT_PATH)/freertos_mmiot
C_INCLUDES += -I$(MMX108_ROOT_PATH)/mm_shims
C_INCLUDES += -I$(MMX108_ROOT_PATH)/mmipal
C_INCLUDES += -I$(MMX108_ROOT_PATH)/mmipal/lwip
C_INCLUDES += -I$(MMX108_ROOT_PATH)/mmpktmem
C_INCLUDES += -I$(MMX108_ROOT_PATH)/mmregdb
C_INCLUDES += -I$(MMX108_ROOT_PATH)/mmutils

CFLAGS += -DHALT_ON_ASSERT

# HaLow SPI backend (pick at most one):
#   (default)     HAL SPI6 in Appli/Core/Src/spi.c + mmhal_wlan.c
#   MMHAL_WLAN_USE_SOFT_SPI — GPIO bit-bang fallback
# CFLAGS += -DMMHAL_WLAN_USE_SOFT_SPI

LDFLAGS += -L$(MMX108_ROOT_PATH) -lmorse
