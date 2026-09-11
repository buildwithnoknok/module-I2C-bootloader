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
 *   0x0000_0000  STAGE-0       1 KB     this code — FROZEN FOREVER (704 B used)
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
 * ── Nothing is erased on unverified data ────────────────────────────────────
 * Before stage-0 touches flash it proves, itself, that the update is real:
 *   1. the control-block descriptor (words 0-4) must match its own CRC32 (word 5)
 *   2. the descriptor must pass the geometry sanity checks
 *   3. the staged image must match the CRC32 stage-1 recorded for it (word 3)
 * Any failure -> "no update pending" -> boot the stage-1 we have not touched.
 * Stage-1 already verifies the image before writing the marker, but stage-0 is
 * the frozen part: it must not have to TRUST that. A corrupt marker, a bit-flip
 * in app_base, or a stage-1 bug can never make this file erase the bootloader
 * region on the strength of bad data.
 *
 * ── Power-loss safety ───────────────────────────────────────────────────────
 * Staging is never erased by the copy, so the copy is always repeatable from a
 * known-good source. Lose power mid-copy and the marker is still set -> we
 * simply do it all again. The operation is idempotent. Any marker value that is
 * not exactly CTRL_MAGIC reads as "no update pending", so a half-written marker
 * fails safe.
 *
 * ── Why every flash op is verified, and what happens when it still fails ───
 * During the DEV-31 bench campaign two transients appeared that never
 * reproduced (one erase that did not take, one hang); prime suspect was bench
 * power. Stage-0 verifies after EVERY erase and program and retries. If a page
 * still will not take — most plausibly a brownout, since flash programming is
 * the highest-current thing this chip does — stage-0 does NOT halt on the first
 * miss: it counts the attempt in no-init RAM and resets. The marker is still
 * set, so the next boot redoes the install on (hopefully) cleaner power. After
 * MAX_ATTEMPTS warm resets it halts. A cold power-on randomises the counter,
 * which reads as "fresh", so a user power-cycle always gets a full new set of
 * attempts. The update path also waits ~50 ms before its first flash access so
 * a slow-ramping rail (DEV-12's lesson) has settled; a normal boot does not.
 *
 * ── Recovery of last resort ─────────────────────────────────────────────────
 * Two states halt the CPU: persistent flash failure across MAX_ATTEMPTS boots,
 * and no stage-1 ever programmed. Both need a hardware fault or a factory
 * error. Stage-0 deliberately never disables SDI, so SWD is always available.
 * There is no LED here on purpose — driving PD1 requires disabling SDI, which
 * would collide with the Flashing Interface V3 SWD window. Stage-1 owns the LED.
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
#define RETRIES            3U            /* per-page erase/program retries       */
#define MAX_ATTEMPTS       4U            /* whole-install retries (warm resets)  */

/* Control block. Written by stage-1, cleared by stage-0. All addresses use the
 * 0x0800_0000 flash-controller alias. Field order is a frozen contract. */
typedef struct {
    uint32_t magic;         /* CTRL_MAGIC = update pending; anything else = no  */
    uint32_t staging_addr;  /* where the new stage-1 image is staged            */
    uint32_t stage1_len;    /* its length in bytes                              */
    uint32_t stage1_crc32;  /* CRC32 of the staged image — verified HERE too    */
    uint32_t app_base;      /* application base — bounds the stage-1 region     */
    uint32_t desc_crc32;    /* CRC32 over the five words above                  */
} ctrl_t;

#define CTRL       ((const volatile ctrl_t *)CTRL_FLASH)
#define DESC_BYTES 20U      /* magic .. app_base */

/* Install-attempt counter in no-init RAM. Lives in the 16 B every linker
 * reserves above the stack (next to stage-1's handoff cell at 0x200007F0), so
 * nothing else writes it and it survives a warm reset. High bits are a tag;
 * if they do not match — cold power-on, random SRAM — the count reads as 0. */
#define ATTEMPT_CELL   (*(volatile uint32_t *)0x200007F4U)
#define ATTEMPT_TAG    0x5A000000U
#define ATTEMPT_MASK   0xFF000000U

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
 * CRC32 — standard zlib (poly 0xEDB88320, init/final 0xFFFFFFFF). Matches
 * stage-1's crc32_buf() and Python's zlib.crc32 on the Pico, so the CRC that
 * stage-1 recorded for the staged image is directly comparable here.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t crc32(uint32_t addr, uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint32_t b = 0; b < 8U; b++)
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : (crc >> 1);
    }
    return ~crc;
}

/* An install attempt failed. Count it and retry from the top on a fresh reset
 * (the marker is still set, so the next boot redoes everything). After
 * MAX_ATTEMPTS consecutive warm-reset attempts, give up and halt — SWD works. */
static void fail_attempt(void)
{
    uint32_t cell = ATTEMPT_CELL;
    uint32_t n    = ((cell & ATTEMPT_MASK) == ATTEMPT_TAG) ? (cell & 0xFFU) : 0U;
    if (n + 1U >= MAX_ATTEMPTS) halt();
    ATTEMPT_CELL = ATTEMPT_TAG | (n + 1U);
    NVIC_SystemReset();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* No SystemInit: the reset-default HSI clock is fine for flash programming,
     * and skipping the PLL removes both code and a failure mode. */

    if (CTRL->magic == CTRL_MAGIC) {

        /* Update path only: let the rail settle before the first flash access.
         * ~50 ms at the reset-default HSI. A normal boot never waits. */
        for (volatile uint32_t i = 0; i < 120000U; i++);

        uint32_t app_base = CTRL->app_base;
        uint32_t staging  = CTRL->staging_addr;
        uint32_t len      = CTRL->stage1_len;

        /* Three gates, cheapest first. Every one must pass before anything is
         * unlocked, let alone erased. */

        /* 1. The descriptor is intact: its own CRC matches. */
        int ok = crc32(CTRL_FLASH, DESC_BYTES) == CTRL->desc_crc32;

        /* 2. The geometry is sane: it can only ever point us at the stage-1
         *    region, and the staging window lies entirely below the control
         *    block. */
        ok = ok &&
            app_base >  STAGE1_BASE_FLASH && app_base <= FLASH_END &&
            (app_base & (PAGE - 1U)) == 0U &&
            len      >  0U   && len <= (app_base - STAGE1_BASE_FLASH) &&
            staging  >= app_base &&
            (staging & (PAGE - 1U)) == 0U &&
            staging + len <= CTRL_FLASH;

        /* 3. The staged image really is what stage-1 verified: recompute its
         *    CRC here. Stage-1 checked it before writing the marker, but the
         *    frozen part does not get to trust the updatable part. */
        ok = ok && crc32(staging, len) == CTRL->stage1_crc32;

        if (ok) {
            uint32_t region = app_base - STAGE1_BASE_FLASH;

            fl_unlock();

            /* Rewrite the whole stage-1 region: the image, then 0xFF to the end
             * so the region is exactly the staged image and nothing stale.
             * A page that will not take after RETRIES is not fatal yet — reset
             * and try the whole install again (marker still set). */
            for (uint32_t off = 0; off < region; off += PAGE) {
                const uint32_t *src = (off < len)
                                    ? (const uint32_t *)(staging + off)
                                    : (const uint32_t *)0;
                if (!write_page(STAGE1_BASE_FLASH + off, src)) fail_attempt();
            }

            /* Clear the marker LAST. Until this lands the update is still
             * pending, so any interruption above simply repeats the copy. */
            for (uint32_t t = 0; t < RETRIES; t++) {
                fl_erase(CTRL_FLASH);
                if (page_matches(CTRL_FLASH, 0)) break;
            }
            if (!page_matches(CTRL_FLASH, 0)) fail_attempt();

            ATTEMPT_CELL = 0;     /* success — next install starts fresh */
            NVIC_SystemReset();
        }
        /* Any gate failed: fall through and boot the stage-1 we have not
         * touched. The marker stays set — stage-0 never clears one it did not
         * act on (that would mean unlocking flash on a normal boot). Stage-1
         * and the host can see it and rewrite it on the next real update. */
    }

    /* Normal boot. Blank-check only: if a stage-1 was ever programmed, run it.
     * Its own validity is stage-1's business, not ours. */
    if (*(const volatile uint32_t *)STAGE1_BASE_FLASH != 0xFFFFFFFFU)
        asm volatile("jr %0" :: "r"(STAGE1_BASE_EXEC));

    halt();   /* no stage-1 ever programmed — factory error; recover over SWD */
    return 0;
}
