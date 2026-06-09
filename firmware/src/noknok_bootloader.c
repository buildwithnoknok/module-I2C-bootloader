/*
 * noknok I2C Module Bootloader  v0.1  (PoC v2 Step 4)
 * CH32V003  |  Stack: cnlohr/ch32fun
 *
 * Re-flashes a noknok CH32V003 module's APPLICATION over the I2C bus — no SWDIO
 * cable needed. The bootloader itself is SWD-flashed ONCE; it never changes in
 * the field. See Confluence "I2C Module Bootloader — Design & Process".
 *
 * ── Flash map (16 KB) ─────────────────────────────────────────────────────
 *   0x0000_0000  BOOTLOADER  (this code, 2 KB)      — runs first on every reset
 *   0x0000_0800  APPLICATION (relinked at offset)   — flashed over I2C
 *   0x0000_3FC0  METADATA (last 64-byte page)       — {magic, len, crc32}
 *
 *   Flash-controller addresses use the 0x0800_0000 alias (same physical flash).
 *
 * ── Boot decision (every reset) ───────────────────────────────────────────
 *   1. handoff magic set in no-init RAM?  -> FLASH MODE (app requested update)
 *   2. else CRC32(app) == metadata.crc?   -> jump to app (normal boot)
 *   3. else                               -> FLASH MODE (blank/corrupt: safe)
 *
 * ── I2C (flash mode) ──────────────────────────────────────────────────────
 *   Address 0x7E. One module flashed at a time.
 *   Master WRITE:
 *     [0x01]                              ERASE app region + metadata
 *     [0x02, offHi, offLo, <64 bytes>]    WRITE_CHUNK (program one 64 B page)
 *     [0x04, len(4 LE), crc32(4 LE)]      VERIFY: CRC32 then write marker
 *     [0x05]                              BOOT (if app valid)
 *   Master READ (2 bytes):  [state, last_error]   (state: 0 IDLE 1 BUSY 2 READY 3 ERROR)
 *
 * ── Status LED ────────────────────────────────────────────────────────────
 *   PD1 (= SWIO), active-low (3V3 -> R -> LED -> PD1). Slow pulse while in
 *   flash mode. Off once the app is running.
 */

#include "ch32fun.h"
#include <string.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BL_I2C_ADDR        0x7E

/* Flash layout (flash-controller alias 0x08000000) */
#define BL_SIZE            0x1000U                  /* 4 KB bootloader region      */
#define APP_BASE_EXEC      (BL_SIZE)                /* 0x00001000 execution/jump   */
#define APP_BASE_FLASH     (0x08000000U + BL_SIZE)  /* FLASH->ADDR / read alias    */
#define APP_REGION_LEN     (0x4000U - BL_SIZE)      /* 12 KB usable for the app    */
#define META_FLASH         (0x08000000U + 0x3FC0U)  /* last 64-byte page           */
#define META_ERASE_PAGE    (0x08000000U + 0x3C00U)  /* 1 KB page holding metadata  */

/* Handoff cell: top 16 bytes of RAM, reserved by the linker (stack ends below) */
#define BL_MAGIC_CELL      (*(volatile uint32_t *)0x200007F0U)
#define BL_MAGIC_ENTER     0x6E6B4231U             /* "nkB1" — please stay in BL  */

/* Metadata validity marker */
#define META_MAGIC         0xB007C0DEU

/* Protocol commands */
#define CMD_ERASE          0x01
#define CMD_WRITE_CHUNK    0x02
#define CMD_VERIFY         0x04
#define CMD_BOOT           0x05

/* Status states */
#define ST_IDLE            0
#define ST_BUSY            1
#define ST_READY           2
#define ST_ERROR           3

#define PAGE_BYTES         64
#define PAGE_WORDS         16

/* ═══════════════════════════════════════════════════════════════════════════
 * METADATA
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t magic;
    uint32_t app_len;
    uint32_t app_crc32;
} meta_t;

#define META  ((const meta_t *)META_FLASH)

/* ═══════════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile uint8_t  bl_state   = ST_IDLE;
static volatile uint8_t  last_error = 0;

static volatile uint8_t  pend_erase = 0;
static volatile uint8_t  pend_write = 0;
static volatile uint8_t  pend_verify= 0;
static volatile uint8_t  pend_boot  = 0;

/* I2C buffers */
#define RX_BUF_SIZE  70           /* WRITE_CHUNK = 3 + 64 = 67 bytes             */
static volatile uint8_t  rx_buf[RX_BUF_SIZE];
static volatile uint8_t  rx_len = 0;

static volatile uint8_t  tx_buf[2];
static volatile uint8_t  tx_idx = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * CRC32  (standard zlib: poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF)
 * Matches Python binascii.crc32 on the Pico side.
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
 * FLASH  (sequence proven in ch32fun examples/flashtest)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void flash_unlock(void)
{
    FLASH->KEYR     = FLASH_KEY1;   /* main program/erase unlock  */
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;   /* fast page program unlock   */
    FLASH->MODEKEYR = FLASH_KEY2;
}

/* Erase one 1 KB page. addr = 0x08000000-based, 1 KB aligned. */
static void flash_erase_1k(uint32_t addr)
{
    FLASH->CTLR  = FLASH_CTLR_PER;
    FLASH->ADDR  = addr;
    FLASH->CTLR  = FLASH_CTLR_PER | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR &= ~FLASH_CTLR_PER;
}

/* Erase whole app region + metadata page (14 × 1 KB). */
static void flash_erase_app(void)
{
    for (uint32_t off = 0; off < APP_REGION_LEN; off += 1024)
        flash_erase_1k(APP_BASE_FLASH + off);
}

/* Fast-program one 64-byte page. addr = 0x08000000-based, 64 B aligned. */
static void flash_program_page(uint32_t addr, const uint32_t *words)
{
    FLASH->CTLR = FLASH_CTLR_PAGE_PG;                       /* fast program mode */
    FLASH->CTLR = FLASH_CTLR_BUF_RST | FLASH_CTLR_PAGE_PG;  /* reset page buffer */
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->ADDR = addr;

    volatile uint32_t *dst = (volatile uint32_t *)addr;
    for (uint8_t i = 0; i < PAGE_WORDS; i++) {
        dst[i]      = words[i];                             /* load into buffer  */
        FLASH->CTLR = FLASH_CTLR_PAGE_PG | FLASH_CTLR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }
    FLASH->CTLR = FLASH_CTLR_PAGE_PG | FLASH_CTLR_STRT;     /* commit page       */
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR &= ~FLASH_CTLR_PAGE_PG;
}

static void flash_write_meta(uint32_t app_len, uint32_t app_crc)
{
    uint32_t words[PAGE_WORDS];
    memset(words, 0xFF, sizeof(words));
    words[0] = META_MAGIC;
    words[1] = app_len;
    words[2] = app_crc;
    flash_erase_1k(META_ERASE_PAGE);         /* metadata lives in last 1 KB page */
    flash_program_page(META_FLASH, words);
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

static void jump_to_app(void)
{
    __disable_irq();
    /* Reset the I2C peripheral so the app starts from a clean slate */
    RCC->APB1PRSTR |=  RCC_APB1Periph_I2C1;
    RCC->APB1PRSTR &= ~RCC_APB1Periph_I2C1;
    /* Jump to the app's reset entry (InterruptVector = "j handle_reset") */
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
    GPIOD->BSHR     =  (1 << 1);   /* drive high = LED off (active-low) */
}

static inline void led_set(uint8_t on)
{
    if (on) GPIOD->BCR  = (1 << 1);   /* low  = on  */
    else    GPIOD->BSHR = (1 << 1);   /* high = off */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * I2C SLAVE  (same peripheral config as the module apps; APB1 = 48 MHz)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void i2c_slave_init(uint8_t addr)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

    /* PC1 = SDA, PC2 = SCL: open-drain alternate function */
    GPIOC->CFGLR &= ~(0xF << (1 * 4)); GPIOC->CFGLR |= (0xF << (1 * 4));
    GPIOC->CFGLR &= ~(0xF << (2 * 4)); GPIOC->CFGLR |= (0xF << (2 * 4));

    I2C1->CTLR1 |=  I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    I2C1->CTLR2  = 48;     /* APB1 = 48 MHz */
    I2C1->CKCFGR = 240;    /* 100 kHz standard mode */
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

    /* Address matched */
    if (star1 & I2C_STAR1_ADDR) {
        rx_len = 0;
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        if (star2 & I2C_STAR2_TRA) {                 /* master is reading */
            tx_buf[0] = bl_state;
            tx_buf[1] = last_error;
            tx_idx    = 1;
            I2C1->DATAR = tx_buf[0];
        }
        return;
    }

    /* Byte received */
    if (star1 & I2C_STAR1_RXNE) {
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (rx_len < RX_BUF_SIZE) rx_buf[rx_len++] = b;
        return;
    }

    /* Transmit buffer empty */
    if (star1 & I2C_STAR1_TXE) {
        I2C1->DATAR = (tx_idx < 2) ? tx_buf[tx_idx++] : 0x00;
        return;
    }

    /* Stop condition -> a write transaction completed */
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
            case CMD_BOOT:
                pend_boot = 1;
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
 * COMMAND PROCESSING  (runs in main loop; flash ops keep state = BUSY)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void do_write_chunk(void)
{
    uint32_t offset = ((uint32_t)rx_buf[1] << 8) | rx_buf[2];
    if (offset + PAGE_BYTES > APP_REGION_LEN) {
        bl_state = ST_ERROR; last_error = 3; return;
    }
    uint32_t words[PAGE_WORDS];
    memcpy(words, (const void *)&rx_buf[3], PAGE_BYTES);   /* word-align the data */
    flash_program_page(APP_BASE_FLASH + offset, words);
    bl_state = ST_READY;
}

static void do_verify(void)
{
    uint32_t len = (uint32_t)rx_buf[1] | ((uint32_t)rx_buf[2] << 8) |
                   ((uint32_t)rx_buf[3] << 16) | ((uint32_t)rx_buf[4] << 24);
    uint32_t crc = (uint32_t)rx_buf[5] | ((uint32_t)rx_buf[6] << 8) |
                   ((uint32_t)rx_buf[7] << 16) | ((uint32_t)rx_buf[8] << 24);
    if (len == 0 || len > APP_REGION_LEN) { bl_state = ST_ERROR; last_error = 4; return; }

    uint32_t actual = crc32_buf((const uint8_t *)APP_BASE_FLASH, len);
    if (actual == crc) {
        flash_write_meta(len, crc);
        bl_state = ST_READY;
    } else {
        bl_state = ST_ERROR; last_error = 5;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    SystemInit();

    uint32_t magic = BL_MAGIC_CELL;
    BL_MAGIC_CELL  = 0;                   /* consume the handoff flag */

    /* Normal boot: no update requested and a valid app is present */
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
        if (pend_boot)   {
            pend_boot = 0;
            if (app_is_valid()) { Delay_Ms(5); jump_to_app(); }
            else { bl_state = ST_ERROR; last_error = 6; }
        }

        /* Slow LED pulse (~3 Hz toggle) using SysTick, non-blocking */
        uint32_t now = SysTick->CNT;
        if ((uint32_t)(now - led_tick) > 1000000U) {   /* ~ HCLK/8 ticks */
            led_tick = now;
            led_on  ^= 1;
            led_set(led_on);
        }
    }
}
