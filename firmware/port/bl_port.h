/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok I2C bootloader - chip port selector (DEV-124)
 * ============================================================================
 * Stage-0 and stage-1 are ONE shared core for every chip: protocol (0x7E,
 * 0xB0/0xB1/0xB3, VERIFY/VERIFY_STAGE1/BOOT), stage-0/stage-1 split, control
 * block, image header, CRC32, layout byte, boot-attempt counters, boot
 * decision. Everything that differs between chips lives in ONE port header:
 * flash map and page size, no-init RAM cells, I2C pins, status LED, watchdog
 * clock, reset-flag policy, image target id.
 *
 *   port_ch32v003.h   CH32V003 I2C modules (buzzer, knob, LED Button, Display)
 *   port_ch32x035.h   CH32X035 power manager of the noknok PicoHub (MOD-0011)
 *
 * The build picks the port with the chip define ch32fun's Makefile sets
 * (CH32V003 / CH32X03x). A port MUST define every BL_* macro the core uses;
 * the core #errors on a missing one rather than guessing.
 */
#ifndef BL_PORT_H
#define BL_PORT_H

#if defined(CH32X03x)
#include "port_ch32x035.h"
#elif defined(CH32V003)
#include "port_ch32v003.h"
#else
#error "noknok bootloader: no port for this chip (CH32V003 or CH32X03x)"
#endif

#if !defined(BL_PAGE) || !defined(BL_FLASH_END) || !defined(BL_CTRL_FLASH) || \
    !defined(BL_META_FLASH) || !defined(BL_RAM_CELLS) || !defined(BL_LAYOUT)
#error "noknok bootloader: the port header is incomplete"
#endif

#endif
