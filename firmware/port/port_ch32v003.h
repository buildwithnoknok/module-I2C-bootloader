/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok I2C bootloader - CH32V003 port (layout 2)
 * ============================================================================
 * Every value here is the one the V003 modules have shipped with since layout 2
 * (11 Sep 2026). Moving them out of the core files (DEV-124) did not change a
 * single byte of the V003 binaries: firmware/bin/noknok_stage0.bin and
 * noknok_stage1.bin rebuild bit-identical (checked with cmp).
 *
 * Flash map (16 KB, 64-byte fast pages):
 *   0x0000_0000  STAGE-0       1 KB     frozen
 *   0x0000_0400  STAGE-1       4 KB     field-updatable
 *   0x0000_1400  APPLICATION   11136 B  (also the stage-1 staging area)
 *   0x0000_3F80  CONTROL BLOCK 64 B
 *   0x0000_3FC0  APP METADATA  64 B
 * No-init RAM cells (top 16 B of the 2 KB RAM): 0x200007F0 handoff,
 * F4 stage-0 attempts, F8 app attempts, FC free.
 */
#ifndef BL_PORT_CH32V003_H
#define BL_PORT_CH32V003_H

#define BL_CHIP_NAME       "CH32V003"

/* ── Flash ─────────────────────────────────────────────────────────────────── */
#define BL_PAGE            64U            /* fast erase + program granularity  */
#define BL_FLASH_END       0x08004000U    /* 16 KB                             */
#define BL_CTRL_FLASH      0x08003F80U    /* stage-1 update control block      */
#define BL_META_FLASH      0x08003FC0U    /* app validity metadata             */
/* VERIFY rewrites the metadata with a 1 KB erase of the sector that holds it
 * (the original monolithic-bootloader code path, kept so the V003 binary stays
 * identical). Note: that sector also holds the top of the app region
 * (0x3C00-0x3F7F), so an app larger than 10 KB would lose its tail here. All
 * V003 apps are far below that today; see the follow-up in DEV-124. */
#define BL_META_ERASE_1K   0x08003C00U

/* ── Stage-1 image header (DEV-31 hardening E) ─────────────────────────────── */
#define BL_LAYOUT          2U             /* 1K stage-0 / 4K stage-1 / app@0x1400 */
#define BL_HDR_OFFSET      0x100U         /* header right after the vector table  */
/* no BL_TARGET: the V003 header is the original 16-byte one (magic, base,
 * layout, version). Layout 2 is a V003-only number, which keeps the other
 * families' images out (they use other layout ids). */

/* ── No-init RAM cells (top 16 B of RAM, reserved by every linker script) ──── */
#define BL_RAM_CELLS       0x200007F0U

/* ── Stage-1 I2C pins: PC1 = SDA, PC2 = SCL, default mapping, AF open-drain ── */
static inline void bl_i2c_pins_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

    GPIOC->CFGLR &= ~(0xF << (1 * 4)); GPIOC->CFGLR |= (0xF << (1 * 4));
    GPIOC->CFGLR &= ~(0xF << (2 * 4)); GPIOC->CFGLR |= (0xF << (2 * 4));
}
#define BL_I2C_CTLR2       48             /* peripheral clock 48 MHz           */

/* ── Status LED: PD1 (SWIO), active-low. Stage-1 owns it (stage-0 never). ──── */
static inline void bl_led_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    GPIOD->CFGLR   &= ~(0xF << (1 * 4));
    GPIOD->CFGLR   |=  ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (1 * 4));
    GPIOD->BSHR     =  (1 << 1);
}
static inline void bl_led_set(uint8_t on)
{
    if (on) GPIOD->BCR  = (1 << 1);
    else    GPIOD->BSHR = (1 << 1);
}
#define BL_LED_TICKS       1000000U       /* SysTick (HCLK/8) ticks per toggle */

/* ── Watchdog armed before the app (stage-1 v1.2.0): LSI ~128 kHz / 64, reload
 *    4095 = ~2.05 s, the setting every layout-2 app uses. ─────────────────────── */
#define BL_IWDG_PSCR       4
#define BL_IWDG_RLDR       0xFFF

/* ── Reset flags: stage-1 reads and clears them on every boot (the app does not
 *    need the cause). ──────────────────────────────────────────────────────────── */
#define BL_RESET_FLAGS_FOR_APP 0

#endif
