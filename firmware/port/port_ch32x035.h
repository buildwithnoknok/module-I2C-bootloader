/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok I2C bootloader - CH32X035 port, layout 3 (DEV-124)
 * ============================================================================
 * Used by the power manager of the noknok PicoHub (MOD-0011, module type 0x07).
 *
 * Flash map (62 KB user flash, 256-byte fast pages, 1 KB standard erase):
 *   0x0000_0000  STAGE-0       1 KB     frozen
 *   0x0000_0400  STAGE-1       4 KB     field-updatable
 *   0x0000_1400  APPLICATION   56 KB    0x1400..0xF3FF (also the staging area)
 *   0x0000_F400  CONTROL BLOCK 256 B    stage-1 update marker
 *   0x0000_F500  APP METADATA  256 B    {magic, app_len, app_crc32}
 *   0x0000_F600  (free, 512 B)
 * No-init RAM cells (top 16 B of the 20 KB RAM): 0x20004FF0 handoff,
 * F4 stage-0 attempts, F8 app attempts, FC free.
 *
 * X035 facts this port depends on (bench-checked where marked):
 *   - Fast page = 256 B: FTER (0x20000) erases one, FTPG (0x10000) programs one
 *     through the 64-word buffer (BUFRST, then per word: store + BUFLOAD, then
 *     ADDR + STRT). Same register sequence as the V003's 64-byte path - only the
 *     page size differs (WCH EVT ch32x035_flash.c, FLASH_ROM_WRITE).
 *   - The WCH factory ROM loader runs first at every power-up and starts user
 *     flash at 0x0000 with a SOFTWARE reset (bench, DEV-117): a cold start shows
 *     POR + SFT in RCC->RSTSCKR. Stage-0 sits at 0x0000 unchanged by that.
 *   - IWDG clock = HSI/1024 = 46.9 kHz (bench, DEV-117), not the V003's LSI.
 *   - mtvec honours only a 1 KB-aligned table base (same QingKe rule as the V003;
 *     stage-1 at 0x0400 and the app at 0x1400 are both 1 KB aligned).
 *   - I2C1 on bus A: remap 0b010 = SCL PC16, SDA PC17 (the QFN-28 has no
 *     default I2C pins). PC16/PC17 are the chip's USB pins, which is why the
 *     factory USB ISP cannot be used on this board.
 */
#ifndef BL_PORT_CH32X035_H
#define BL_PORT_CH32X035_H

#define BL_CHIP_NAME       "CH32X035"

/* ── Flash ─────────────────────────────────────────────────────────────────── */
#define BL_PAGE            256U
#define BL_FLASH_END       0x0800F800U    /* 62 KB                             */
#define BL_CTRL_FLASH      0x0800F400U
#define BL_META_FLASH      0x0800F500U
/* ch32fun's X035 header spells the buffer bits without the underscore */
#ifndef FLASH_CTLR_BUF_LOAD
#define FLASH_CTLR_BUF_LOAD FLASH_CTLR_BUFLOAD
#endif
#ifndef FLASH_CTLR_BUF_RST
#define FLASH_CTLR_BUF_RST  FLASH_CTLR_BUFRST
#endif

/* ── Image headers ───────────────────────────────────────────────────────────
 * Layout 3 = the X035 map above. Layout ids are unique across chip families,
 * so the brain's exact layout gate never pairs a V003 image with an X035 and
 * the V003 stage-1 (which wants layout 2) refuses an X035 stage-1 image.
 * The X035 headers also carry a target id, checked by this stage-1, for both
 * a staged stage-1 image (error 8) and an application image (error 9). */
#define BL_LAYOUT          3U
#define BL_HDR_OFFSET      0x200U         /* past the X035 vector table (312 B) */
#define BL_TARGET          0x35333058U    /* "X035" little-endian               */

/* ── No-init RAM cells ─────────────────────────────────────────────────────── */
#define BL_RAM_CELLS       0x20004FF0U

/* ── Stage-1 I2C pins: bus A, I2C1 remap 0b010 = SCL PC16, SDA PC17 ────────── */
static inline void bl_i2c_pins_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_I2C1_REMAP) | AFIO_PCFR1_I2C1_REMAP_1;
    /* PC16/PC17 live in CFGXR (pins 16..23): AF open-drain */
    GPIOC->CFGXR = (GPIOC->CFGXR & ~0xFFu) | 0xFFu;
}
#define BL_I2C_CTLR2       48             /* peripheral clock 48 MHz           */

/* ── Status LED D4: PB12 = LED_K, active-low ───────────────────────────────── */
static inline void bl_led_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;
    GPIOB->BSHR   = (1 << 12);                               /* off           */
    GPIOB->CFGHR  = (GPIOB->CFGHR & ~(0xFu << 16)) | (0x1u << 16);  /* PP 10 MHz */
}
static inline void bl_led_set(uint8_t on)
{
    if (on) GPIOB->BCR  = (1 << 12);
    else    GPIOB->BSHR = (1 << 12);
}
/* A slow, even blink (0.5 s on / 0.5 s off) = "in the bootloader", unlike any
 * of the app's patterns (4 s blip, 2 s double blip, 8 Hz protection blink). */
#define BL_LED_TICKS       3000000U       /* SysTick (HCLK/8 = 6 MHz) ticks    */

/* ── Watchdog armed before the app: 46.9 kHz / 64, reload 1465 = ~2.0 s.
 *    The app re-arms it to ~1 s (/32, 1465) at start-up. ───────────────────── */
#define BL_IWDG_PSCR       4
#define BL_IWDG_RLDR       1465

/* ── Reset flags ─────────────────────────────────────────────────────────────
 * The PicoHub app NEEDS the reset cause: a cold start (POR) holds the 5 V buck
 * off while it negotiates USB-PD; a warm start must never touch the buck (it
 * would reboot the Pico). So on the boot path stage-1 only READS the flags and
 * leaves them for the app, which clears them itself. Stage-1 clears them only
 * when it stays in flash mode: an app installed later is then started as a
 * WARM start (flags empty), because by then the Pico is running from the buck.
 * Three-strikes still works: the app clears the flags at every start, so an
 * IWDG flag seen here is always from the reset that just happened. */
#define BL_RESET_FLAGS_FOR_APP 1

#endif
