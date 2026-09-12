/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * Silent application — bench harness for the stage-1 v1.2.0 watchdog contract
 * (DEV-31, 12 Sep 2026).
 *
 * The failure this reproduces: an application with a VALID CRC that dies before
 * it ever starts its own watchdog. That is what a wrongly-linked image does
 * (a layout-2 build pushed onto a layout-1 module faulted before iwdg_init()
 * and hung SILENTLY — not parked, because hardening C only counts WATCHDOG
 * resets and there was no watchdog). Recovery needed the SWD clamp.
 *
 * Since v1.2.0 stage-1 arms the IWDG itself before jumping, so this app —
 * which stamps a witness and then spins, never touching the IWDG at all —
 * must be reset ~2 s later, counted, and parked at 0x7E with error 7 after
 * three tries. Same harness as bad_app.c minus the three IWDG lines; the
 * witness stamp is 0x5A (bad_app uses 0xBA) so the two can never be confused.
 *
 * Expect after ~7 s: witness bytes [1] [2] [3] == 5A, byte [4] == FF, and the
 * module answering at 0x7E with status [3, 7]. On a pre-1.2.0 stage-1 this
 * app hangs forever and 0x7E stays silent — that is the FAIL signature.
 *
 * Linked at 0x1400 (fake_app.ld), reuses stage-0's minimal start.S.
 */

#include "ch32fun.h"
#include <stdint.h>

#define WITNESS     0x08003C00U
#define PAGE_WORDS  16U
#define ATTEMPT_CELL (*(volatile uint32_t *)0x200007F8U)
#define ATTEMPT_TAG  0xA5000000U

static void fl_unlock(void)
{
    FLASH->KEYR     = FLASH_KEY1;
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

static void fl_program(uint32_t addr, const uint32_t *src)
{
    FLASH->CTLR = FLASH_CTLR_PAGE_PG;
    FLASH->CTLR = FLASH_CTLR_BUF_RST | FLASH_CTLR_PAGE_PG;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->ADDR = addr;

    volatile uint32_t *dst = (volatile uint32_t *)addr;
    for (uint32_t i = 0; i < PAGE_WORDS; i++) {
        dst[i]      = src[i];
        FLASH->CTLR = FLASH_CTLR_PAGE_PG | FLASH_CTLR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }
    FLASH->CTLR  = FLASH_CTLR_PAGE_PG | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->STATR = FLASH_STATR_EOP;
    FLASH->CTLR  = 0;
}

int main(void)
{
    uint32_t c = ATTEMPT_CELL;
    uint32_t n = ((c & 0xFF000000U) == ATTEMPT_TAG) ? (c & 0xFFU) : 0U;
    if (n > 15U) n = 15U;

    uint32_t page[PAGE_WORDS];
    for (uint32_t i = 0; i < PAGE_WORDS; i++) page[i] = 0xFFFFFFFFU;
    ((volatile uint8_t *)page)[n] = 0x5A;

    fl_unlock();
    fl_program(WITNESS, page);

    /* NO watchdog of our own. Just hang. Stage-1's IWDG must get us. */
    for (;;);
}
