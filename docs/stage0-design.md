<!--
SPDX-License-Identifier: MIT
Copyright (c) noknok
-->

# Stage-0 / Stage-1 Bootloader Split — Design Spec

**Status:** design, bench-validated flash behaviour · **Jira:** DEV-31 · **Target:** CH32V003 (CH32V203 variant at the end)

This is the design for making the noknok module **bootloader itself** field-updatable.
Everything else on a module already is: the payload firmware updates over I2C/USB, and
the Conductor (`noknok.py`) + `product.py` update via the manifest. The bootloader was
the one component frozen at manufacture — this closes that gap, so anything we decide
after crowdfunding can still be rolled out to backer units over the air.

---

## 1. The problem

The chip always begins executing at address `0x0000_0000`, and **you cannot erase the
flash page you are currently executing from**. A single monolithic bootloader therefore
cannot rewrite itself in place without copying a stub to RAM — and if power drops while
the bootloader region is half-erased and the RAM stub is gone, the module is bricked and
only SWD recovers it. DEV-31 explicitly forbids that outcome.

## 2. The approach: a frozen stage-0 plus an updatable stage-1

| | Role |
|---|---|
| **Stage-0** | Tiny, frozen forever, at `0x0000`. No bus, no interrupts, no clock setup. Its only jobs: apply a pending stage-1 update, and jump to stage-1. |
| **Stage-1** | The real bootloader — today's I2C/USB OTA logic, behaviour unchanged. Field-updatable by stage-0. |
| **Application** | The module's payload firmware. Unchanged, still flashed by stage-1. |

**Stage-0 never speaks a bus.** A new stage-1 image is received by the *running stage-1*
into a staging area, and stage-0 only ever does a dumb, verified flash copy. That is what
keeps stage-0 small enough to be correct by inspection, and it is why the same design
works for USB (where a receiver would mean carrying a whole USB stack in the frozen part).

## 3. Flash map (CH32V003, 16 KB)

| Region | Address | Size | Written by | Mutable in field |
|---|---|---|---|---|
| **Stage-0** | `0x0000` | 512 B | SWD, once at manufacture | **Never** |
| **Stage-1** | `0x0200` | 3.5 KB | stage-0, from staging | Yes |
| **Application** | `0x1000` | 12 KB − 128 B | stage-1, over the bus | Yes |
| **BL control block** | `0x3F80` | 64 B | stage-1 (set) / stage-0 (clear) | Yes |
| **App metadata** | `0x3FC0` | 64 B | stage-1 | Yes |

The whole split fits inside the **existing 4 KB bootloader reservation**, so the
application base stays at `0x1000` and **existing module apps need no relinking**.

The control block and app metadata are two separate 64-byte pages inside the same 1 KB
sector. Keeping them separate means stage-0 can clear the update marker without
destroying app metadata — which relies on 64-byte erase granularity (validated, §6).

### Stage-0 hardcodes exactly two addresses

`STAGE1_BASE` (`0x0200`) and `CTRL_BLOCK` (`0x3F80`). **Everything else is data it reads
from the control block** — including the application base and the staging address.

This is what keeps DEV-31's question 2 answered "yes": a future OTA *can* enlarge the
bootloader reservation (e.g. stage-1 `0x0200`→`0x1800`, trading payload space), because
stage-0 never assumes where the application lives. Such a resize is necessarily a
combined "new stage-1 + new app" transaction, since an app linked at `0x1000` cannot run
at a different base. The permanent floor is stage-0's own 512 B.

### Control block layout (`0x3F80`, 64 B)

| Word | Field | Meaning |
|---|---|---|
| 0 | `update_magic` | `0x6E6B5530` = stage-1 update pending; anything else = none |
| 1 | `staging_addr` | where the new stage-1 image is staged (normally `0x0800_1000`) |
| 2 | `staging_len` | length in bytes |
| 3 | `staging_crc32` | zlib CRC32, verified by stage-1 *before* the marker is written |
| 4 | `app_base` | application base address — read by stage-0, never assumed |
| 5 | `stage1_len` | current stage-1 length (informational / `GET_BL_VERSION`) |

Any value in word 0 other than the exact magic means "no update pending". That is the
fail-safe direction: a half-written marker reads as garbage → treated as no update →
stage-0 jumps to a stage-1 it has not touched.

## 4. Boot decision (stage-0, every reset)

1. Read the control block. If `update_magic` matches **and** the staging descriptor is
   sane → **apply the stage-1 update** (§5), then reset.
2. Else if stage-1's first word is not `0xFFFFFFFF` → **jump to stage-1** at `0x0200`.
3. Else → no stage-1 was ever programmed. Signal on the status LED and halt; SWD
   recovery required. This state is reachable only by a factory error, never by a field
   event.

Stage-0 does **not** look at the app, the handoff cell, or the app's CRC — all of that
stays stage-1's job. Keeping the two strictly separated is what holds stage-0 small.

## 5. Stage-1 update sequence

1. Host sends the running app `0xB0` → app resets into the bootloader.
2. Host sends the new stage-1 image to stage-1, targeted at the **staging area** (the app
   region). Reuses the existing `ERASE` / `WRITE_CHUNK` machinery, different destination.
3. Host sends `VERIFY_STAGE1 {len, crc32}`. Stage-1 CRC-checks the staged image.
4. **Only on a CRC match**, stage-1 writes the control block and warm-resets.
5. Stage-0 sees the marker, erases the stage-1 region, copies staging → stage-1 with
   verify-and-retry per page, clears the marker (64-byte erase of `0x3F80`), and resets.
6. New stage-1 boots. The app region now holds the staging copy, so `app_is_valid()`
   fails and stage-1 waits in flash mode — the host pushes the application as normal.

The application is always destroyed by a bootloader update. That is deliberate and
acceptable: the app is re-pushed immediately afterwards, and it is what lets us stage
into the app region instead of permanently reserving scratch space.

### Power-loss analysis — every window recovers

| Power lost during | State on next boot | Recovery |
|---|---|---|
| Staging transfer (2–3) | No marker; stage-1 intact | Boots stage-1 → flash mode → host retries |
| Marker write (4) | Marker garbage → reads as "no update"; stage-1 untouched | Boots stage-1 → flash mode → host retries |
| Erase / copy (5) | Marker still set; **staging never touched** | Stage-0 redoes the erase+copy — idempotent |
| Marker clear (5) | Marker garbage → "no update"; stage-1 now valid | Boots the new stage-1 |

There is **no window in which the module becomes unrecoverable without physical access**,
which is DEV-31's core acceptance criterion. Staging is CRC-verified before the marker is
ever set, and is never erased during the copy, so the copy is always repeatable from a
known-good source.

## 6. Bench-validated flash behaviour (10 Sep 2026)

The layout above depends on the CH32V003's **64-byte fast erase**
(`FLASH_CTLR_PAGE_ER`, bit 17). Probed on an LED Button board via WCH-LinkE:

- **1664 erases, zero failures.**
- **Erases exactly 64 bytes.** Every neighbouring page retained its exact tag across a
  checkerboard pattern, in every round, including across all 1 KB sector boundaries.
- **The CPU keeps executing from a sector while a page in that same sector is erased**
  (192 consecutive erases inside the live-code sector). This is the specific behaviour
  a 512-byte stage-0 needs, since it must erase `0x0200` from within sector 0.
- `BSY` asserts immediately after `STRT` (256/256) — no settle delay is required.

### Erase sequence to freeze into stage-0

```c
FLASH->CTLR  = FLASH_CTLR_PAGE_ER;
FLASH->ADDR  = addr;
FLASH->CTLR  = FLASH_CTLR_PAGE_ER | FLASH_CTLR_STRT;
while (FLASH->STATR & FLASH_STATR_BSY);
FLASH->STATR = FLASH_STATR_EOP;   /* write-1-to-clear      */
FLASH->CTLR  = 0;                 /* plain write, never RMW */
```

### Two unexplained transients — and why stage-0 must not trust an erase

During the same session, two anomalies occurred that never reproduced:

1. One erase silently did not take (1 of the first 4 attempted); not seen again in 1664.
2. One run hung before writing its results; did not recur when the configuration was
   reproduced deliberately.

Neither is explained by the code. The most likely cause is bench power: the LED Button
**must be powered from a JST-SH pigtail, not the pogo clamp** (its LED boost sits
upstream of R1, 10R), and flash erase is the highest-current operation the chip performs.

Regardless of cause, the conclusion for a component that can never be patched is:

> **Stage-0 MUST verify every page after erase and after program, and retry on mismatch.
> It must never assume a flash operation succeeded.**

This costs roughly 30 bytes and makes the entire class of failure a non-event.

**Open item:** re-run the probe on pigtail power. If the transients persist there, this
design needs revisiting before the production batch is programmed.

## 7. Stage-0 size budget — the one open risk

512 B is a target, not yet a measured result. Estimated content:

| Part | Est. |
|---|---|
| Startup / vector (no `SystemInit`, reset-default clock) | ~40 B |
| Flash unlock | ~30 B |
| 64-byte erase | ~40 B |
| 64-byte program | ~80 B |
| Copy loop + verify + retry | ~120 B |
| Control-block read + validation | ~60 B |
| Blank check + jump to stage-1 | ~30 B |
| **Total** | **~400 B** |

Deliberately excluded to hold the budget: clock/PLL setup (flash programming is fine at
the reset-default HSI), interrupts and NVIC, any bus stack, and CRC32 — the marker state
machine plus a blank check gives the recovery guarantee, and CRC in stage-0 would buy
detection without recovery.

**Because 64-byte erase is proven, the boundary is not a binary 512-vs-1024 choice.**
Stage-0's reservation can be any multiple of 64 bytes. The rule is therefore:

> Build stage-0, measure it, round up to the next 64-byte boundary, add one page of
> margin, and **freeze that number before DEV-29 programs the production batch.**

Recommendation: freeze at **512 B** if it fits, **768 B** if not. Either way stage-1 has
comfortable room over its expected ~2.4 KB.

## 8. Stage-1 changes required

- Accept a stage-1 image into the staging area (new command; reuses existing transfer).
- `VERIFY_STAGE1 {len, crc32}` → write the control block only on match, then warm-reset.
- **`GET_BL_VERSION`** — new, and load-bearing: neither bootloader can currently report a
  version. The I2C bootloader has none at all; the USB bootloader has `BL_VERSION` in
  source but no command to read it. Without this the Conductor would be pushing
  bootloader updates blind. Should report stage-0 presence + stage-1 version.

## 9. CH32V203 (USB modules)

Same design, more headroom, and no dependency on the fast-erase result:

| Region | Address | Size |
|---|---|---|
| Stage-0 | `0x0800_0000` | 1 KB |
| Stage-1 | `0x0800_0400` | 7 KB |
| Application | `0x0800_2000` | ~23.75 KB |

The V203 uses proven 1 KB erases throughout, so stage-0 never erases within its own
sector. Stage-0 must keep the DEV-12 power-settle delay before any flash access.

## 10. Sequencing

Stage-0 must be finalised and bench-validated **before DEV-29 programs the production
batch** — it is the one component with no second chance. Coordinate with DEV-22
(Flashing Interface V3): same batch, same constraint, same bench session.
