# noknok stage-1 — one source for every chip (DEV-124); the chip port is
# ../port/port_<chip>.h:
#   make                 CH32V003 -> noknok_stage1.bin
#   make CHIP=x035       CH32X035 -> noknok_stage1_x035.bin (noknok PicoHub)
#   make CHIP=x035 EXTRA_CFLAGS=-DS1_VERSION_PATCH=1   a test 1.0.1 image
#
# Relinked to 0x0400, above the frozen 1 KB stage-0. Unlike stage-0, stage-1
# links ch32fun.c — it needs the interrupt vector table for the I2C handlers.

CHIP ?= v003

ifeq ($(CHIP),v003)
TARGET        = noknok_stage1
TARGET_MCU   ?= CH32V003
LINKER_SCRIPT := stage1.ld
else ifeq ($(CHIP),x035)
# noknok_stage1_x035.c only #includes noknok_stage1.c: ch32fun.mk derives the
# source file from TARGET, and a separate TARGET keeps the two chips' build
# outputs (.elf/.bin/.map) apart.
TARGET        = noknok_stage1_x035
TARGET_MCU   := CH32X035
TARGET_MCU_PACKAGE := CH32X035G8U6
LINKER_SCRIPT := stage1_x035.ld
else
$(error CHIP must be v003 or x035)
endif

override EXTRA_CFLAGS += -I../port   # kept when EXTRA_CFLAGS is also given on the command line
EXTRA_ELF_DEPENDENCIES += stage1_sections.ld noknok_stage1.c ../port/bl_port.h \
                          ../port/port_ch32v003.h ../port/port_ch32x035.h

# ch32fun checkout: the bench layout has it next to this folder
CH32FUN_MK ?= ../ch32fun/ch32fun.mk
include $(CH32FUN_MK)

all   : build
flash : cv_flash
clean : cv_clean
