TARGET  = noknok_stage1

TARGET_MCU ?= CH32V003

# Relinked to 0x0300, above the frozen 768 B stage-0. Unlike stage-0, stage-1
# links ch32fun.c — it needs the interrupt vector table for the I2C handlers.
LINKER_SCRIPT := stage1.ld

include ../ch32fun/ch32fun.mk

all   : build
flash : cv_flash
clean : cv_clean
