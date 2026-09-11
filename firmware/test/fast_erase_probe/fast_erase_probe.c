/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * CH32V003 64-byte fast-erase probe  (DEV-31)
 *
 * Answers the question the stage-0 / stage-1 split depends on: does
 * FLASH_CTLR_PAGE_ER erase exactly 64 bytes, leaving neighbouring 64-byte pages
 * in the SAME 1 KB sector intact — including while the CPU is executing from
 * that sector?
 *
 * If yes, a sub-1 KB stage-0 is possible on erase grounds. (It turned out the
 * binding constraint is mtvec alignment, not erase granularity - see the spec.)
 * If no, stage-0 must be a full 1 KB (sector granularity).
 *
 * Method, repeated ROUNDS times:
 *   1. clear the working pages (64-byte erases only)
 *   2. tag every 64-byte page with its own address
 *   3. checkerboard: erase every ODD-indexed page — an erase boundary on BOTH
 *      sides of every surviving page
 *   4. verify on chip: odd pages blank, even pages still holding their tag
 *
 * Padding forces the image past 0x0400 so sector 1 (0x0400-0x07FF) holds live
 * code while we erase pages at 0x0600-0x07FF inside that same sector. Set
 * SEC_BASE/SEC_END to 0x1000/0x1200 instead to test a sector with no code.
 *
 * Results block at 0x08002000 (LE words), read over SWD:
 *   minichlink -r + 0x08002000 28
 *   [0] 0xD09EC0DE marker - ran to completion (absence == hang)
 *   [1] erase_fail  - odd pages not fully erased     (want 0)
 *   [2] keep_fail   - even pages that lost their tag (want 0)
 *   [3] first_fail  - address of first failure, else 0xFFFFFFFF
 *   [4] erase_count [5] rounds [6] 0xDEADBEEF (padding was really in flash)
 *
 * Build/run on the Pi bench host:
 *   cp -r . /home/noknok/dev/ch32fun/noknok_erasetest && cd there
 *   make build && make flash && sleep 20
 *   ../minichlink/minichlink -r + 0x08002000 28
 *
 * NOTE: power the module from a JST-SH pigtail, NOT the pogo clamp. Flash erase
 * is the highest-current operation the chip performs and clamp power is
 * marginal on the LED Button (LED boost upstream of R1, 10R). Two unexplained
 * transients during the 10 Sep 2026 session are suspected to come from this.
 *
 * Result 10 Sep 2026: 1664 erases, zero failures, isolation perfect, and the
 * CPU kept fetching from a sector under erase. See docs/stage0-design.md §6.
 */

#include "ch32fun.h"
#include <stdint.h>

#define PAGE_BYTES   64
#define PAGE_WORDS   16

#define SEC_BASE     0x08000600U   /* inside the live-code sector */
#define SEC_END      0x08000800U

#define MARKER_ADDR  0x08002000U
#define MARKER_WORD  0xD09EC0DEU
#define ROUNDS       16

/* Pushes the image past 0x0400 so sector 1 definitely contains live code. */
__attribute__((used, section(".rodata.pad")))
static const uint32_t pad[64] = { 0xDEADBEEFU };

static uint32_t erase_count = 0;

static void flash_unlock(void)
{
    FLASH->KEYR     = FLASH_KEY1;
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

static void flash_erase_1k(uint32_t addr)
{
    FLASH->CTLR = FLASH_CTLR_PER;
    FLASH->ADDR = addr;
    FLASH->CTLR = FLASH_CTLR_PER | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
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
    FLASH->CTLR = FLASH_CTLR_PAGE_PG | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
}

/* The sequence intended for stage-0: plain CTLR writes, EOP cleared, no RMW. */
static void flash_erase_64(uint32_t addr)
{
    FLASH->CTLR  = FLASH_CTLR_PAGE_ER;
    FLASH->ADDR  = addr;
    FLASH->CTLR  = FLASH_CTLR_PAGE_ER | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->STATR = FLASH_STATR_EOP;
    FLASH->CTLR  = 0;
    erase_count++;
}

int main(void)
{
    SystemInit();
    flash_unlock();

    uint32_t words[PAGE_WORDS];
    uint32_t erase_fail = 0, keep_fail = 0, first_fail = 0xFFFFFFFFU;
    uint32_t rounds = 0;

    for (uint32_t r = 0; r < ROUNDS; r++) {

        /* 64-byte erases only — a 1 KB erase here would destroy this program's
         * own code lower in the same sector. */
        for (uint32_t a = SEC_BASE; a < SEC_END; a += PAGE_BYTES)
            flash_erase_64(a);

        for (uint32_t a = SEC_BASE; a < SEC_END; a += PAGE_BYTES) {
            uint32_t tag = 0xA5000000U | (a & 0x0000FFFFU);
            for (uint8_t i = 0; i < PAGE_WORDS; i++) words[i] = tag;
            flash_program_page(a, words);
        }

        for (uint32_t a = SEC_BASE; a < SEC_END; a += PAGE_BYTES)
            if ((a >> 6) & 1U) flash_erase_64(a);

        for (uint32_t a = SEC_BASE; a < SEC_END; a += PAGE_BYTES) {
            uint32_t tag = 0xA5000000U | (a & 0x0000FFFFU);
            const volatile uint32_t *p = (const volatile uint32_t *)a;
            uint32_t odd = (a >> 6) & 1U;

            for (uint8_t i = 0; i < PAGE_WORDS; i++) {
                uint32_t v = p[i];
                if (odd ? (v != 0xFFFFFFFFU) : (v != tag)) {
                    if (odd) erase_fail++; else keep_fail++;
                    if (first_fail == 0xFFFFFFFFU) first_fail = a;
                    break;
                }
            }
        }
        rounds++;
    }

    flash_erase_1k(MARKER_ADDR);
    for (uint8_t i = 0; i < PAGE_WORDS; i++) words[i] = 0;
    words[0] = MARKER_WORD;
    words[1] = erase_fail;
    words[2] = keep_fail;
    words[3] = first_fail;
    words[4] = erase_count;
    words[5] = rounds;
    words[6] = pad[0];
    flash_program_page(MARKER_ADDR, words);

    while (1);
}
