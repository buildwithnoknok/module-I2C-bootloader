/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * Fake stage-1 — bench harness for testing stage-0 (DEV-31).
 *
 * Stands in for the real stage-1 so stage-0 can be validated before stage-1
 * exists. All it does is stamp its own ID into a witness page and halt, so a
 * single SWD read tells us WHICH stage-1 actually ran.
 *
 * Build it twice with different -DSTAGE1_ID values:
 *   ID 0xA1 = the stage-1 already installed at 0x0400
 *   ID 0xA2 = the replacement staged in the app region for stage-0 to copy
 *
 * Then reading the witness page answers the only question that matters:
 *   0xA1 -> stage-0 jumped to the installed stage-1  (jump path works)
 *   0xA2 -> stage-0 applied the pending update       (update path works)
 *
 * Linked at 0x0400 (fake_stage1.ld) and reuses stage-0's minimal start.S.
 */

#include "ch32fun.h"
#include <stdint.h>

#ifndef STAGE1_ID
#define STAGE1_ID 0x000000A1U
#endif

#define WITNESS   0x08003C00U          /* inside the app region, clear of staging */
#define PAGE_WORDS 16U

static void fl_unlock(void)
{
    FLASH->KEYR     = FLASH_KEY1;
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

static void fl_erase(uint32_t addr)
{
    FLASH->CTLR  = FLASH_CTLR_PAGE_ER;
    FLASH->ADDR  = addr;
    FLASH->CTLR  = FLASH_CTLR_PAGE_ER | FLASH_CTLR_STRT;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->STATR = FLASH_STATR_EOP;
    FLASH->CTLR  = 0;
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
    uint32_t page[PAGE_WORDS];
    for (uint32_t i = 0; i < PAGE_WORDS; i++) page[i] = STAGE1_ID;

    fl_unlock();
    fl_erase(WITNESS);
    fl_program(WITNESS, page);

    for (;;);
}
