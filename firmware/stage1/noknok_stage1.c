/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok Stage-1  v1.0.0   CH32V003   |  Stack: cnlohr/ch32fun
 * ============================================================================
 * The real bootloader: re-flashes a module's APPLICATION over I2C, exactly as
 * the monolithic bootloader always did. What is new (DEV-31) is that stage-1 is
 * itself FIELD-UPDATABLE — it can receive its own replacement and hand it to
 * the frozen stage-0 below it to install.
 *
 * Relinked from 0x0000 to 0x0300. Derived from firmware/src/noknok_bootloader.c
 * (2132 B, hardware-validated Jun 2026); the app-flashing path is unchanged.
 *
 * Design spec: docs/stage0-design.md   ·   Jira: DEV-31
 *
 * ── Flash map (16 KB) ───────────────────────────────────────────────────────
 *   0x0000_0000  STAGE-0       768 B    frozen; applies stage-1 updates
 *   0x0000_0300  STAGE-1       3.25 KB  this code — updatable via stage-0
 *   0x0000_1000  APPLICATION   ~11.9 KB flashed over I2C; also the STAGING area
 *   0x0000_3F80  CONTROL BLOCK 64 B     stage-1 update marker, read by stage-0
 *   0x0000_3FC0  METADATA      64 B     {magic, app_len, app_crc32}
 *
 * ── Boot decision (stage-0 has already run) ─────────────────────────────────
 *   1. handoff magic set in no-init RAM?  -> FLASH MODE (app requested update)
 *   2. else CRC32(app) == metadata.crc?   -> jump to app (normal boot)
 *   3. else                               -> FLASH MODE (blank/corrupt: safe)
 *
 * ── I2C (flash mode, address 0x7E) ──────────────────────────────────────────
 *   Master WRITE:
 *     [0x01]                              ERASE app region + metadata
 *     [0x02, offHi, offLo, <64 bytes>]    WRITE_CHUNK (program one 64 B page)
 *     [0x04, len(4 LE), crc32(4 LE)]      VERIFY app: CRC32 then write marker
 *     [0x06, len(4 LE), crc32(4 LE)]      VERIFY_STAGE1: CRC32 then arm update
 *     [0x05]                              BOOT (app, or reset to apply stage-1)
 *     [0xB1]                              GET_VERSION -> next read gives 4 bytes
 *   Master READ:  [state, last_error], or 4 version bytes after 0xB1.
 *
 * ── How a stage-1 update works ──────────────────────────────────────────────
 * The transfer is IDENTICAL to an application update — same ERASE, same
 * WRITE_CHUNK, same staging area. Only the closing command differs:
 *
 *   app update:      ERASE -> WRITE_CHUNK xN -> VERIFY(0x04)        -> BOOT
 *   stage-1 update:  ERASE -> WRITE_CHUNK xN -> VERIFY_STAGE1(0x06) -> BOOT
 *
 * VERIFY_STAGE1 CRC-checks the staged image and, only on a match, writes the
 * control block that tells stage-0 an update is pending. BOOT then resets, and
 * stage-0 does the install. The application is destroyed in the process (the
 * staging area IS the app region) and is re-pushed afterwards — deliberate, and
 * what avoids permanently reserving scratch flash on a 16 KB part.
 *
 * Nothing is armed until the CRC matches, so a failed or interrupted transfer
 * simply leaves no marker and the module boots this stage-1 unchanged.
 */

#include "ch32fun.h"
#include <string.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BL_I2C_ADDR        0x7E

/* Version reported by GET_VERSION (0xB1). BL_PROTOCOL_VERSION describes the
 * bootloader command set; the triple is this stage-1's own semver.
 * A module that ANSWERS 0xB1 at 0x7E is a stage-0/stage-1 module. The old
 * monolithic bootloader does not implement it, so silence means "old world" —
 * that is how the Conductor tells the two fleets apart. */
#define BL_PROTOCOL_VERSION 0x01
#define S1_VERSION_MAJOR    1
#define S1_VERSION_MINOR    0
#define S1_VERSION_PATCH    0

/* Flash layout (flash-controller alias 0x08000000) */
#define APP_BASE_EXEC      0x00001000U
#define APP_BASE_FLASH     0x08001000U

/* Two different lengths, deliberately:
 *   APP_REGION_LEN  how many bytes an APPLICATION may occupy. Stops at the
 *                   control block so an oversized app can never overwrite the
 *                   stage-1 update marker. Bounds WRITE_CHUNK and the CRC.
 *   APP_ERASE_LEN   how much ERASE actually clears — the whole 12 KB through
 *                   the end of flash, so the control block and the app metadata
 *                   are both wiped when a fresh image is loaded.
 * They differ by 128 B (the control block + metadata pages). Keeping them as
 * separate named constants makes that intentional rather than a rounding
 * accident in the erase loop. */
#define APP_REGION_LEN     (0x3F80U - 0x1000U)      /* 12160 B usable          */
#define APP_ERASE_LEN      (0x4000U - 0x1000U)      /* 12 KB, 12 x 1 KB pages  */
#define META_FLASH         (0x08000000U + 0x3FC0U)  /* app validity marker      */
#define META_ERASE_PAGE    (0x08000000U + 0x3C00U)  /* 1 KB page holding it     */
#define CTRL_FLASH         (0x08000000U + 0x3F80U)  /* stage-1 update control   */

/* Handoff cell: top 16 bytes of RAM, reserved by the linker (stack ends below) */
#define BL_MAGIC_CELL      (*(volatile uint32_t *)0x200007F0U)
#define BL_MAGIC_ENTER     0x6E6B4231U             /* "nkB1" — stay in the BL   */

#define META_MAGIC         0xB007C0DEU
#define CTRL_MAGIC         0x6E6B5530U             /* "nkU0" — update pending   */

/* Protocol commands */
#define CMD_ERASE          0x01
#define CMD_WRITE_CHUNK    0x02
#define CMD_VERIFY         0x04
#define CMD_BOOT           0x05
#define CMD_VERIFY_STAGE1  0x06
#define CMD_GET_VERSION    0xB1

/* Status states */
#define ST_IDLE            0
#define ST_BUSY            1
#define ST_READY           2
#define ST_ERROR           3

#define PAGE_BYTES         64
#define PAGE_WORDS         16

/* ═══════════════════════════════════════════════════════════════════════════
 * FLASH STRUCTURES
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t magic;
    uint32_t app_len;
    uint32_t app_crc32;
} meta_t;

/* Read by stage-0. Field order is a frozen contract — see docs/stage0-design.md. */
typedef struct {
    uint32_t magic;         /* CTRL_MAGIC = a stage-1 update is pending */
    uint32_t staging_addr;
    uint32_t stage1_len;
    uint32_t stage1_crc32;
    uint32_t app_base;      /* bounds the stage-1 region for stage-0    */
} ctrl_t;

#define META  ((const meta_t *)META_FLASH)
#define CTRL  ((const volatile ctrl_t *)CTRL_FLASH)

/* ═══════════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile uint8_t  bl_state   = ST_IDLE;
static volatile uint8_t  last_error = 0;

static volatile uint8_t  pend_erase  = 0;
static volatile uint8_t  pend_write  = 0;
static volatile uint8_t  pend_verify = 0;
static volatile uint8_t  pend_vs1    = 0;
static volatile uint8_t  pend_boot   = 0;

#define RX_BUF_SIZE  70           /* WRITE_CHUNK = 3 + 64 = 67 bytes */
static volatile uint8_t  rx_buf[RX_BUF_SIZE];
static volatile uint8_t  rx_len = 0;

static volatile uint8_t  tx_buf[4];
static volatile uint8_t  tx_idx = 0;
static volatile uint8_t  tx_len = 2;   /* 2 = [state,err]; 4 after GET_VERSION */
static volatile uint8_t  version_pending = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * CRC32  (standard zlib: poly 0xEDB88320, init 0xFFFFFFFF, final XOR)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320U : (crc >> 1);
    }
    return ~crc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FLASH
 * ═══════════════════════════════════════════════════════════════════════════ */

static void flash_unlock(void)
{
    FLASH->KEYR     = FLASH_KEY1;
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

static void flash_erase_1k(uint32_t addr)
{
    FLASH->CTLR  = FLASH_CTLR_PER;
    FLASH->ADDR  = addr;
    FLASH->CTLR  = FLASH_CTLR_PER | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->STATR = FLASH_STATR_EOP;
    FLASH->CTLR  = 0;
}

/* 64-byte fast erase — bench-validated DEV-31 (1664 erases, zero failures).
 * Lets us rewrite the control block without disturbing the metadata page next
 * to it in the same 1 KB sector. */
static void flash_erase_64(uint32_t addr)
{
    FLASH->CTLR  = FLASH_CTLR_PAGE_ER;
    FLASH->ADDR  = addr;
    FLASH->CTLR  = FLASH_CTLR_PAGE_ER | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->STATR = FLASH_STATR_EOP;
    FLASH->CTLR  = 0;
}

static void flash_erase_app(void)
{
    for (uint32_t off = 0; off < APP_ERASE_LEN; off += 1024)
        flash_erase_1k(APP_BASE_FLASH + off);
}

static void flash_program_page(uint32_t addr, const uint32_t *words)
{
    FLASH->CTLR = FLASH_CTLR_PAGE_PG;
    FLASH->CTLR = FLASH_CTLR_BUF_RST | FLASH_CTLR_PAGE_PG;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->ADDR = addr;

    volatile uint32_t *dst = (volatile uint32_t *)addr;
    for (uint8_t i = 0; i < PAGE_WORDS; i++) {
        dst[i]      = words[i];
        FLASH->CTLR = FLASH_CTLR_PAGE_PG | FLASH_CTLR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }
    FLASH->CTLR  = FLASH_CTLR_PAGE_PG | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->STATR = FLASH_STATR_EOP;
    FLASH->CTLR  = 0;
}

static void flash_write_meta(uint32_t app_len, uint32_t app_crc)
{
    uint32_t words[PAGE_WORDS];
    memset(words, 0xFF, sizeof(words));
    words[0] = META_MAGIC;
    words[1] = app_len;
    words[2] = app_crc;
    flash_erase_1k(META_ERASE_PAGE);
    flash_program_page(META_FLASH, words);
}

/* Arm a stage-1 update for stage-0. Written ONLY after the staged image has
 * passed its CRC — until this lands, nothing is pending. */
static void flash_write_ctrl(uint32_t len, uint32_t crc)
{
    uint32_t words[PAGE_WORDS];
    memset(words, 0xFF, sizeof(words));
    words[0] = CTRL_MAGIC;
    words[1] = APP_BASE_FLASH;    /* staging_addr — the app region */
    words[2] = len;
    words[3] = crc;
    words[4] = APP_BASE_FLASH;    /* app_base — bounds the stage-1 region */
    flash_erase_64(CTRL_FLASH);   /* 64 B erase leaves metadata alone */
    flash_program_page(CTRL_FLASH, words);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * APP VALIDITY
 * ═══════════════════════════════════════════════════════════════════════════ */

static int app_is_valid(void)
{
    if (META->magic != META_MAGIC)        return 0;
    if (META->app_len == 0)               return 0;
    if (META->app_len > APP_REGION_LEN)   return 0;
    uint32_t crc = crc32_buf((const uint8_t *)APP_BASE_FLASH, META->app_len);
    return (crc == META->app_crc32);
}

static int stage1_update_pending(void)
{
    return CTRL->magic == CTRL_MAGIC;
}

static void jump_to_app(void)
{
    __disable_irq();
    RCC->APB1PRSTR |=  RCC_APB1Periph_I2C1;
    RCC->APB1PRSTR &= ~RCC_APB1Periph_I2C1;
    asm volatile("jr %0" :: "r"(APP_BASE_EXEC));
    while (1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SWIO STATUS LED  (PD1, active-low)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void led_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    GPIOD->CFGLR   &= ~(0xF << (1 * 4));
    GPIOD->CFGLR   |=  ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (1 * 4));
    GPIOD->BSHR     =  (1 << 1);
}

static inline void led_set(uint8_t on)
{
    if (on) GPIOD->BCR  = (1 << 1);
    else    GPIOD->BSHR = (1 << 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * I2C SLAVE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void i2c_slave_init(uint8_t addr)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

    GPIOC->CFGLR &= ~(0xF << (1 * 4)); GPIOC->CFGLR |= (0xF << (1 * 4));
    GPIOC->CFGLR &= ~(0xF << (2 * 4)); GPIOC->CFGLR |= (0xF << (2 * 4));

    I2C1->CTLR1 |=  I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    I2C1->CTLR2  = 48;
    I2C1->CKCFGR = 240;
    I2C1->OADDR1 = ((uint16_t)addr << 1);

    I2C1->CTLR2 |= I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITERREN;
    I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_PE;

    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
}

void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void)
{
    uint32_t star1 = I2C1->STAR1;
    uint32_t star2 = I2C1->STAR2;   /* reading STAR2 clears ADDR */

    if (star1 & I2C_STAR1_ADDR) {
        rx_len = 0;
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        if (star2 & I2C_STAR2_TRA) {              /* master is reading */
            if (version_pending) {
                version_pending = 0;
                tx_buf[0] = BL_PROTOCOL_VERSION;
                tx_buf[1] = S1_VERSION_MAJOR;
                tx_buf[2] = S1_VERSION_MINOR;
                tx_buf[3] = S1_VERSION_PATCH;
                tx_len    = 4;
            } else {
                tx_buf[0] = bl_state;
                tx_buf[1] = last_error;
                tx_len    = 2;
            }
            tx_idx      = 1;
            I2C1->DATAR = tx_buf[0];
        }
        return;
    }

    if (star1 & I2C_STAR1_RXNE) {
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (rx_len < RX_BUF_SIZE) rx_buf[rx_len++] = b;
        return;
    }

    if (star1 & I2C_STAR1_TXE) {
        I2C1->DATAR = (tx_idx < tx_len) ? tx_buf[tx_idx++] : 0x00;
        return;
    }

    if (star1 & I2C_STAR1_STOPF) {
        I2C1->CTLR1 |= I2C_CTLR1_PE;   /* clears STOPF */

        if (rx_len >= 1) {
            switch (rx_buf[0]) {
            case CMD_ERASE:
                bl_state = ST_BUSY; pend_erase = 1;
                break;
            case CMD_WRITE_CHUNK:
                if (rx_len == 3 + PAGE_BYTES) { bl_state = ST_BUSY; pend_write = 1; }
                else { bl_state = ST_ERROR; last_error = 1; }
                break;
            case CMD_VERIFY:
                if (rx_len == 1 + 8) { bl_state = ST_BUSY; pend_verify = 1; }
                else { bl_state = ST_ERROR; last_error = 2; }
                break;
            case CMD_VERIFY_STAGE1:
                if (rx_len == 1 + 8) { bl_state = ST_BUSY; pend_vs1 = 1; }
                else { bl_state = ST_ERROR; last_error = 2; }
                break;
            case CMD_BOOT:
                pend_boot = 1;
                break;
            case CMD_GET_VERSION:
                version_pending = 1;
                break;
            default:
                break;
            }
        }
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return;
    }
}

void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void)
{
    I2C1->STAR1 &= ~(I2C_STAR1_BERR | I2C_STAR1_ARLO |
                     I2C_STAR1_AF   | I2C_STAR1_OVR);
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * COMMAND PROCESSING  (main loop; flash ops keep state = BUSY)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void do_write_chunk(void)
{
    uint32_t offset = ((uint32_t)rx_buf[1] << 8) | rx_buf[2];
    if (offset + PAGE_BYTES > APP_REGION_LEN) {
        bl_state = ST_ERROR; last_error = 3; return;
    }
    uint32_t words[PAGE_WORDS];
    memcpy(words, (const void *)&rx_buf[3], PAGE_BYTES);
    flash_program_page(APP_BASE_FLASH + offset, words);
    bl_state = ST_READY;
}

/* Shared by VERIFY and VERIFY_STAGE1: both CRC the staging area. */
static int staged_crc_ok(uint32_t *len_out, uint32_t max_len)
{
    uint32_t len = (uint32_t)rx_buf[1] | ((uint32_t)rx_buf[2] << 8) |
                   ((uint32_t)rx_buf[3] << 16) | ((uint32_t)rx_buf[4] << 24);
    uint32_t crc = (uint32_t)rx_buf[5] | ((uint32_t)rx_buf[6] << 8) |
                   ((uint32_t)rx_buf[7] << 16) | ((uint32_t)rx_buf[8] << 24);

    if (len == 0 || len > max_len) { bl_state = ST_ERROR; last_error = 4; return 0; }

    if (crc32_buf((const uint8_t *)APP_BASE_FLASH, len) != crc) {
        bl_state = ST_ERROR; last_error = 5; return 0;
    }
    *len_out = len;
    return 1;
}

static void do_verify(void)
{
    uint32_t len;
    if (!staged_crc_ok(&len, APP_REGION_LEN)) return;

    uint32_t crc = (uint32_t)rx_buf[5] | ((uint32_t)rx_buf[6] << 8) |
                   ((uint32_t)rx_buf[7] << 16) | ((uint32_t)rx_buf[8] << 24);
    flash_write_meta(len, crc);
    bl_state = ST_READY;
}

/* Arm a stage-1 self-update. The staged image must fit the stage-1 region,
 * which is bounded by the application base. */
static void do_verify_stage1(void)
{
    uint32_t len;
    if (!staged_crc_ok(&len, APP_BASE_FLASH - 0x08000300U)) return;

    uint32_t crc = (uint32_t)rx_buf[5] | ((uint32_t)rx_buf[6] << 8) |
                   ((uint32_t)rx_buf[7] << 16) | ((uint32_t)rx_buf[8] << 24);
    flash_write_ctrl(len, crc);
    bl_state = ST_READY;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    SystemInit();

    uint32_t magic = BL_MAGIC_CELL;
    BL_MAGIC_CELL  = 0;                   /* consume the handoff flag */

    if (magic != BL_MAGIC_ENTER && app_is_valid())
        jump_to_app();                    /* never returns */

    /* ── FLASH MODE ── */
    flash_unlock();
    led_init();
    i2c_slave_init(BL_I2C_ADDR);
    bl_state = ST_IDLE;

    __enable_irq();

    uint32_t led_tick = 0;
    uint8_t  led_on   = 0;

    while (1)
    {
        if (pend_erase)  { pend_erase = 0; flash_erase_app(); bl_state = ST_READY; }
        if (pend_write)  { pend_write = 0; do_write_chunk(); }
        if (pend_verify) { pend_verify = 0; do_verify(); }
        if (pend_vs1)    { pend_vs1 = 0; do_verify_stage1(); }
        if (pend_boot)   {
            pend_boot = 0;
            /* A pending stage-1 update outranks booting the app — the app region
             * holds the staged image anyway. Reset so stage-0 installs it. */
            if (stage1_update_pending()) { Delay_Ms(5); NVIC_SystemReset(); }
            else if (app_is_valid())     { Delay_Ms(5); jump_to_app(); }
            else { bl_state = ST_ERROR; last_error = 6; }
        }

        uint32_t now = SysTick->CNT;
        if ((uint32_t)(now - led_tick) > 1000000U) {
            led_tick = now;
            led_on  ^= 1;
            led_set(led_on);
        }
    }
}
