TARGET  = noknok_bootloader

TARGET_MCU ?= CH32V003

# Use our custom 2 KB-bootloader linker instead of the generated full-flash one.
LINKER_SCRIPT := bootloader.ld

include ../ch32fun/ch32fun.mk

all   : build
flash : cv_flash
clean : cv_clean
