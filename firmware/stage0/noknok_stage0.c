/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok Stage-0  v1.0.0   CH32V003   |  Stack: cnlohr/ch32fun
 * ============================================================================
 * THIS COMPONENT IS FROZEN. Once the production batch is programmed (DEV-29) it
 * can never be changed in the field. Everything above it — stage-1, the module
 * application, the Conductor — is updatable. Stage-0 is the thing that makes
 * that true, so it must stay small enough to be correct by inspection.
 *
 * Design spec: docs/stage0-design.md   ·   Jira: DEV-31
 *
 * ── What it does ────────────────────────────────────────────────────────────
 * Exactly two jobs:
 *   1. If stage-1 has a pending update, apply it (verified copy from staging).
 *   2. Jump to stage-1.
 * It does NOT speak I2C, look at the application, check the app CRC, read the
 * handoff cell, enable interrupts, set up the PLL, or touch PD1. All of that is
 * stage-1's job. That separation is what keeps this file small.
 *
 * ── Flash map (16 KB) ───────────────────────────────────────────────────────
 *   0x0000_0000  STAGE-0       1 KB     this code — FROZEN FOREVER (500 B used)
 *   0x0000_0400  STAGE-1       3 KB     the real bootloader — updatable
 *   0x0000_1000  APPLICATION   ~11.9 KB the module firmware — updatable
 *   0x0000_3F80  CONTROL BLOCK 64 B     stage-1 update marker (below)
 *   0x0000_3FC0  APP METADATA  64 B     stage-1 owns this, stage-0 ignores it
 *
 * Stage-0 hardcodes exactly TWO addresses: STAGE1_BASE and CTRL_FLASH.
 * Everything else — where the app starts, where staging lives, how long the new
 * stage-1 is — is DATA read from the control block. That is deliberate: it is
 * what lets a future OTA move the stage-1/application boundary without needing
 * to change this frozen file.
 *
 * ── Power-loss safety ───────────────────────────────────────────────────────
 * Staging is CRC-verified by stage-1 BEFORE the marker is written, and is never
 * erased by the copy, so the copy is always repeatable from a known-good source.
 * Lose power mid-copy and the marker is still set -> we simply do it all again.
 * The operation is idempotent. Any marker value that is not exactly CTRL_MAGIC
 * reads as "no update pending", so a half-written marker fails safe: we jump to
 * a stage-1 we had not yet touched.
 *
 * ── Why every flash op is verified ──────────────────────────────────────────
 * During the DEV-31 bench campaign (1664 erases, zero failures) two transients
 * appeared that never reproduced: one erase that silently did not take, and one
 * hang. Prime suspect is bench power, not silicon. For a component that can
 * never be patched, the correct response is not to explain them away but to
 * make them survivable: stage-0 verifies after EVERY erase and EVERY program
 * and retries. Costs ~30 bytes, and turns that whole class of fault into a
 * non-event.
 *
 * ── Recovery of last resort ─────────────────────────────────────────────────
 * Two states halt the CPU: unrecoverable flash failure, and no stage-1 ever
 * programmed. Both are reachable only by a hardware fault or a factory error,
 * never by a field event. Stage-0 deliberately never disables SDI, so SWD is
 * always available to reflash the part. There is no LED here on purpose —
 * driving PD1 requires disabling SDI, which would collide with the Flashing
 * Interface V3 SWD window (DEV-22). Stage-1 owns the LED.
 */

#include "ch32fun.h"
#include <stdint.h>

/* ── The two frozen addresses ─────────────────────────────────────────────── */
#define STAGE1_BASE_EXEC   0x00000400U   /* execution alias — where we jump    */
#define STAGE1_BASE_FLASH  0x08000400U   /* controller alias — where we write  */
#define CTRL_FLASH         0x08003F80U   /* stage-1 update control block       */

/* ── Fixed properties of the part ─────────────────────────────────────────── */
#define FLASH_END          0x08004000U   /* 16 KB                              */
#define PAGE               64U           /* erase + program granularity        */
#define PAGE_WORDS         (PAGE / 4U)

#define CTRL_MAGIC         0x6E6B5530U   /* "nkU0" — a stage-1 update is pending */
#define RETRIES            3U

/* Control block. Written by stage-1, cleared by stage-0. All addresses use the
 * 0x0800_0000 flash-controller alias. */
typedef struct {
    uint32_t magic;         /* CTRL_MAGIC = update pending; anything else = no  */
    uint32_t staging_addr;  /* where the new stage-1 image is staged            */
    uint32_t stage1_len;    /* its length in bytes                              */
    uint32_t stage1_crc32;  /* checked by stage-1 before the marker; unused here */
    uint32_t app_base;      /* application base — bounds the stage-1 region     */
} ctrl_t;

#define CTRL ((const volatile ctrl_t *)CTRL_FLASH)

/* ═══════════════════════════════════════════════════════════════════════════
 * FLASH  (CH32V003 fast 64-byte page path — bench-validated, DEV-31)
 * Plain CTLR writes, EOP cleared, never read-modify-write.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fl_unlock(void)
{
    FLASH->KEYR     = FLASH_KEY1;   /* main program/erase unlock */
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;   /* fast page mode unlock     */
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

/* Compare one page against src, or against 0xFF if src is NULL. */
static int page_matches(uint32_t addr, const uint32_t *src)
{
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    for (uint32_t i = 0; i < PAGE_WORDS; i++) {
        uint32_t want = src ? src[i] : 0xFFFFFFFFU;
        if (p[i] != want) return 0;
    }
    return 1;
}

/* Erase, optionally program, then VERIFY. Retries. Returns 0 if it never took. */
static int write_page(uint32_t addr, const uint32_t *src)
{
    for (uint32_t t = 0; t < RETRIES; t++) {
        fl_erase(addr);
        if (src) fl_program(addr, src);
        if (page_matches(addr, src)) return 1;
    }
    return 0;
}

static void halt(void)
{
    /* Unrecoverable. SDI was never disabled, so SWD can always reflash. */
    for (;;);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* No SystemInit: the reset-default HSI clock is fine for flash programming,
     * and skipping the PLL removes both code and a failure mode. */

    if (CTRL->magic == CTRL_MAGIC) {

        uint32_t app_base = CTRL->app_base;
        uint32_t staging  = CTRL->staging_addr;
        uint32_t len      = CTRL->stage1_len;

        /* Validate the descriptor before letting it drive an erase. A bad
         * control block must never be able to point us at the wrong region. */
        int sane =
            app_base >  STAGE1_BASE_FLASH && app_base <= FLASH_END &&
            (app_base & (PAGE - 1U)) == 0U &&
            len      >  0U   && len <= (app_base - STAGE1_BASE_FLASH) &&
            staging  >= app_base &&
            (staging & (PAGE - 1U)) == 0U &&
            staging + len <= CTRL_FLASH;

        if (sane) {
            uint32_t region = app_base - STAGE1_BASE_FLASH;

            fl_unlock();

            /* Rewrite the whole stage-1 region: the image, then 0xFF to the end
             * so the region is exactly the staged image and nothing stale. */
            for (uint32_t off = 0; off < region; off += PAGE) {
                const uint32_t *src = (off < len)
                                    ? (const uint32_t *)(staging + off)
                                    : (const uint32_t *)0;
                if (!write_page(STAGE1_BASE_FLASH + off, src)) halt();
            }

            /* Clear the marker LAST. Until this lands the update is still
             * pending, so any interruption above simply repeats the copy. */
            for (uint32_t t = 0; t < RETRIES; t++) {
                fl_erase(CTRL_FLASH);
                if (page_matches(CTRL_FLASH, 0)) break;
            }

            NVIC_SystemReset();
        }
        /* Not sane: fall through and boot stage-1. The marker stays set, but a
         * descriptor this broken would only fail again — stage-1 and the host
         * can see it and rewrite it. */
    }

    /* Normal boot. Blank-check only: if a stage-1 was ever programmed, run it.
     * Its own validity is stage-1's business, not ours. */
    if (*(const volatile uint32_t *)STAGE1_BASE_FLASH != 0xFFFFFFFFU)
        asm volatile("jr %0" :: "r"(STAGE1_BASE_EXEC));

    halt();   /* no stage-1 ever programmed — factory error; recover over SWD */
    return 0;
}
