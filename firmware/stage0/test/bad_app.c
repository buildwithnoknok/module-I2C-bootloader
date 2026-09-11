/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * Bad application — bench harness for hardening C (DEV-31).
 *
 * Simulates the field failure that matters most: an application with a VALID
 * CRC that hangs every time it runs. Before hardening C such an app was booted
 * forever and the module was dead until SWD. Now stage-1 counts the attempts
 * and, after APP_MAX_ATTEMPTS, parks in flash mode with last_error = 7.
 *
 * What this does on every boot:
 *   1. read stage-1's boot-attempt counter from no-init RAM (0x200007F8)
 *   2. stamp 0xBA into witness byte [attempt] so we can see afterwards exactly
 *      how many times we ran (programming without erase: FF -> BA is a legal
 *      1->0 transition and leaves earlier stamps alone)
 *   3. start the independent watchdog, then hang without ever kicking it
 * ~2 s later the IWDG resets the chip; stage-1 counts one more attempt.
 *
 * Expect after ~7 s: witness bytes [1] [2] [3] == BA, byte [4] == FF (never a
 * fourth run), and the module answering at 0x7E with status [3, 7].
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
    uint32_t c = ATTEMPT_CELL;
    uint32_t n = ((c & 0xFF000000U) == ATTEMPT_TAG) ? (c & 0xFFU) : 0U;
    if (n > 15U) n = 15U;

    /* One page of 0xFF with a single 0xBA at byte [n]. Programming over the
     * existing page ANDs in, so previous stamps survive. */
    uint32_t page[PAGE_WORDS];
    for (uint32_t i = 0; i < PAGE_WORDS; i++) page[i] = 0xFFFFFFFFU;
    ((volatile uint8_t *)page)[n] = 0xBA;

    fl_unlock();
    fl_program(WITNESS, page);

    /* Start the watchdog (~2 s) and never feed it. */
    IWDG->CTLR = 0x5555;  IWDG->PSCR = 4;
    IWDG->CTLR = 0x5555;  IWDG->RLDR = 0xFFF;
    IWDG->CTLR = 0xCCCC;
    for (;;);
}
