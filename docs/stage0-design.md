<!--
SPDX-License-Identifier: MIT
Copyright (c) noknok
-->

# Stage-0 / Stage-1 Bootloader Split — Design Spec

**Status:** implemented and validated over I²C (11 Sep 2026); frozen stage-0 1 KB / stage-1 at `0x0400`; **layout 2** (stage-1 4 KB, app `0x1400`) since 11 Sep 2026; **stage-1 v1.2.0** (12 Sep 2026: IWDG armed before the app, `0xB1` reports layout — §8, "Watchdog contract") · **Jira:** DEV-31 · **Target:** CH32V003 (CH32V203 variant at the end)

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
| **Stage-0** | `0x0000` | 1 KB (704 B used) | SWD, once at manufacture | **Never** |
| **Stage-1** | `0x0400` | **4 KB** (2928 B used, v1.1.0) | stage-0, from staging | Yes |
| **Application** | `0x1400` | 11 KB − 128 B (11136 B) | stage-1, over the bus | Yes |
| **BL control block** | `0x3F80` | 64 B | stage-1 (set) / stage-0 (clear) | Yes |
| **App metadata** | `0x3FC0` | 64 B | stage-1 | Yes |

**Layout history.** Layout 1 (10–11 Sep 2026) kept the split inside the legacy 4 KB
reservation: stage-1 3 KB, app at `0x1000`, existing apps unchanged. The DEV-31 hardening
round took stage-1 to 2908 B (164 B free) with DEV-22's flashing-interface logic still to
come, so on 11 Sep 2026 Christopher approved growing it: **layout 2** = stage-1 **4 KB**,
app base **`0x1400`**, all four apps relinked (LED Button 2.4.0, Buzzer 3.5.0, Knob 2.3.0,
Display 0.5.0). Stage-0 was not touched — it reads the app base from the control block,
which is exactly the property this section was written to guarantee. The stage-1 image
header's layout id went 1 → 2 so `VERIFY_STAGE1` refuses a cross-layout image (error 8).
The fleet was clean-slate, so no field migration was needed; the runbook's combined
"new stage-1 + new app" transaction remains the procedure if a layout ever moves again.

The control block and app metadata are two separate 64-byte pages inside the same 1 KB
sector. Keeping them separate means stage-0 can clear the update marker without
destroying app metadata — which relies on 64-byte erase granularity (validated, §6).

### Stage-0 hardcodes exactly two addresses

`STAGE1_BASE` (`0x0400`) and `CTRL_BLOCK` (`0x3F80`). **Everything else is data it reads
from the control block** — including the application base and the staging address.

This is what keeps DEV-31's question 2 answered "yes": a future OTA *can* enlarge the
bootloader reservation (as layout 2 did: stage-1 3 → 4 KB, trading payload space), because
stage-0 never assumes where the application lives. Such a resize is necessarily a
combined "new stage-1 + new app" transaction, since an app linked at one base cannot run
at a different base. The permanent floor is stage-0's own 1 KB.

### Control block layout (`0x3F80`, 64 B)

| Word | Field | Meaning |
|---|---|---|
| 0 | `update_magic` | `0x6E6B5530` = stage-1 update pending; anything else = none |
| 1 | `staging_addr` | where the new stage-1 image is staged (normally `0x0800_1000`) |
| 2 | `staging_len` | length in bytes |
| 3 | `staging_crc32` | zlib CRC32, verified by stage-1 *before* the marker is written |
| 4 | `app_base` | application base address — read by stage-0, never assumed |
| 5 | `desc_crc32` | zlib CRC32 over words 0–4 — **stage-0 gate 1** (hardening A, 11 Sep) |

Any value in word 0 other than the exact magic means "no update pending". That is the
fail-safe direction: a half-written marker reads as garbage → treated as no update →
stage-0 jumps to a stage-1 it has not touched.

## 4. Boot decision (stage-0, every reset)

1. Read the control block. If `update_magic` matches, the update path opens — but
   **nothing is unlocked until three gates pass**, cheapest first:
   1. **Descriptor CRC** — words 0–4 must match word 5. A corrupt or half-written marker
      is rejected here.
   2. **Geometry** — `app_base` above stage-1 and inside flash, page-aligned; `len` fits
      the region; staging lies entirely below the control block.
   3. **Staged image CRC** — stage-0 recomputes CRC32 over `(staging, len)` and requires
      it to equal word 3. Stage-1 verified the image before writing the marker, but the
      frozen part does not get to *trust* the updatable part.
   All three pass → **apply the stage-1 update** (§5), then reset. Any gate fails → fall
   through as if no update were pending; the marker is left set (clearing it would mean
   unlocking flash on a normal boot) and stage-1 rewrites it on the next real update.
2. Else if stage-1's first word is not `0xFFFFFFFF` → **jump to stage-1** at `0x0400`.
3. Else → no stage-1 was ever programmed. Halt; SWD recovery required. This state is
   reachable only by a factory error, never by a field event.

**Why the CRC in stage-0 after all.** The first design left CRC32 out of stage-0 ("detection
without recovery"). That was right for checking the *installed* stage-1 on every boot — there
is nothing to fall back to. It was wrong for checking the *staged* image before erasing:
that is prevention, and the recovery is trivial (don't erase; boot the old one). Before
hardening A, a descriptor could be constructed that passed every geometry check and would
have erased most of flash and installed 64 bytes of junk. Now a corrupt marker, a bit-flip
in `app_base`, a stage-1 bug, or staging corruption after the marker was written can never
make the frozen component erase the bootloader region. Cost: ~130 B, one CRC routine used
twice.

**Install failure retries by reset (hardening B).** If a page will not take after its
per-page retries — most plausibly a brownout, flash programming being the highest-current
thing the chip does — stage-0 counts the attempt in no-init RAM (`0x200007F4`, in the 16 B
every linker reserves above the stack, next to the handoff cell) and resets. The marker is
still set, so the next boot redoes the install on cleaner power. After **4** consecutive
warm-reset attempts it halts. A cold power-on randomises the cell, which reads as "fresh",
so a user power-cycle always gets a full new set of attempts. The update path also waits
~50 ms before its first flash access (DEV-12's brownout lesson); a normal boot never waits.

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
| Staging transfer (2–3) | No marker; stage-1 intact | Boots stage-1 → flash mode → host retries. **Proven** (`test_stage_cut`) |
| Marker write (4) | Marker garbage → reads as "no update"; stage-1 untouched | Boots stage-1 → flash mode → host retries. **Proven** for both a missing magic (`test_marker_nomagic`) and a landed magic with an insane descriptor (`test_marker_garbage` — rejected, marker deliberately left set) |
| Erase / copy (5) | Marker still set; **staging never touched** | Stage-0 redoes the erase+copy — idempotent. **Proven** (`test_recover`) |
| Marker clear (5) | Marker still set; stage-1 already complete | Stage-0 redoes the copy (idempotent), clears, boots the new stage-1. **Proven** (`test_clear_cut`) |

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
  a sub-1 KB stage-0 would need. (It turned out not to be the binding constraint — see §7.)
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

## 7. Stage-0 size — MEASURED (10 Sep 2026; hardened 11 Sep)

**Stage-0 is implemented and measures 704 B** after hardening A + B (it was 500 B before
CRC32 and retry-by-reset were added — +204 B for a CRC routine used twice, the attempt
counter, and the settle delay). 320 B of the 1 KB reservation remain. Source:
`firmware/stage0/`. The size history below is the pre-hardening build, kept because the
lesson in it still applies.

The first build came out at **936 B** — nearly double the estimate — and the reason is
worth recording: **248 B of it was `ch32fun.c`**, which contributes a full 38-entry
interrupt vector table and a general-purpose reset handler. Stage-0 never enables an
interrupt, so none of that could ever run.

Stage-0 therefore **does not link `ch32fun.c` at all**. It has its own 8-byte reset entry
(`start.S`) and uses `ch32fun.h` for register definitions only (header-only, no code).
That single change saved 436 B. It is also the right call independently: in a component
we can never patch, every symbol we don't link is one that can't surprise us later.

| Part | Measured |
|---|---|
| `.init` — reset entry (`start.S`) | 8 B |
| `fl_erase` (kept out of line) | 36 B |
| `main` — everything else, inlined by LTO | 436 B |
| Padding | 20 B |
| **Total** | **500 B** |
| RAM | **0 B** |

Excluded to hold the budget: clock/PLL setup (flash programming is fine at the
reset-default HSI), interrupts and NVIC, any bus stack, and an LED. CRC32 was *originally*
excluded too, on the reasoning that checking the installed stage-1 would be detection
without recovery — that reasoning was right for that use and wrong for verifying the staged
image before erasing, so hardening A brought it in (§4).

Stage-0 uses **zero RAM** — all state is locals. `start.S` therefore does not copy `.data`
or zero `.bss`, and `stage0.ld` **ASSERTs both are empty** so a future edit that
introduces a global fails the build loudly instead of running on uninitialised memory.

### The frozen boundary: 1 KB — and why it is not smaller

The image measures 500 B, and §6 proved 64-byte erase granularity, so on erase grounds a
512 B or 768 B reservation would have worked. **It does not, for a different reason:**

> **The stage-1 base must be 1 KB aligned.** On this core `mtvec` only honours the upper
> address bits. ch32fun sets `mtvec = InterruptVector | 3`; with stage-1 linked at `0x0300`
> the hardware silently used `0x0000`, so the first I²C interrupt jumped into stage-0's
> code and the CPU fell over.

**How it presented on the bench (11 Sep 2026):** stage-1 at `0x0300` ACKed its I²C address
in hardware — the peripheral was configured and matching — but every read returned all
`0xFF`, because the ISR that loads the data register never ran. Relinked to `0x0400` it
worked first time, unchanged. Empirically: 256 B alignment fails, 1 KB works; 512 B was not
tested and is not worth the risk for a frozen component.

**Why none of the 10 Sep tests caught it:** none of them took an interrupt in stage-1. The
boot-chain test found a valid app and jumped before any I²C traffic. The first real bus
transaction found it in seconds — which is exactly why the I²C bench session exists.

Since nothing can live in `0x0300–0x03FF` if stage-1 cannot start there, stage-0 takes the
whole 1 KB. 524 B of slack covers pre-freeze fixes and toolchain drift many times over.
Stage-1 gets **3 KB against a measured 2600 B** (472 B free) — comfortable, and stage-1 is
updatable anyway. This is the "Layout A" from the original design discussion; the 64-byte
erase finding remains real and useful (the control block depends on it, and it is what lets
a future OTA move `app_base` at fine granularity), it just was not the binding constraint.

Both linker scripts now `ASSERT` this: `stage1.ld` checks its base is 1 KB aligned as a
separate assertion, so the guard survives if the base is ever moved again.

> **Frozen boundary: stage-0 = 1 KB, stage-1 base = `0x0400`.**
> This is the one number that cannot change after DEV-29 programs the production batch.
> Superseded figures: 512 B / `0x0200` (design estimate), 768 B / `0x0300` (first measured
> build, before the alignment constraint was known).

## 7A. Bench validation of stage-0 (10 Sep 2026, re-run 11 Sep at `0x0400`)

Validated on an LED Button board over SWD, using a fake stage-1 harness
(`firmware/stage0/test/`) that stamps its own ID into a witness page at `0x3C00`, so a
single read says *which* stage-1 actually ran. Two builds: `A1` = already installed,
`A2` = the replacement. All three images are generated by `build_test_images.py`.

| Test | Setup | Expected | Result |
|---|---|---|---|
| **Jump path** | stage-0 + A1 installed, no update pending | witness `0xA1` | **PASS** |
| **Update path** | + A2 staged at `0x1000`, marker set | witness `0xA2`, marker cleared | **PASS** |
| **Recovery** | stage-1 **half-written**, staging intact, marker still set, witness pre-cleared to `0x00` | witness `0xA2` | **PASS** |
| **Boot chain** | stage-0 + the **real** stage-1 + a fake app with valid metadata | witness `0xA3` | **PASS** |

Also verified after the update: the stage-1 region is byte-identical to `fake_a2.bin`, and
the region tail past the image reads `0xFF` — stage-0 rewrites the whole region, leaving
nothing stale.

The recovery test is the important one: it reproduces exactly what a power cut mid-copy
leaves behind, and stage-0 noticed the update was still pending and simply did the whole
copy again. The second half — the part missing after the simulated cut — came back
byte-identical. **This is DEV-31's core acceptance criterion demonstrated on hardware.**

The boot-chain test is the integration proof: stage-0 hands off to the **real** stage-1 at
`0x0400`, which runs, validates the application against its metadata, and jumps to it.

All four were re-run on 11 Sep after the move to `0x0400`: 4/4 again.

## 7B. I²C bench validation — the real thing (11 Sep 2026)

Pico + `brain-Pico/software/bench_stage1.py`, LED Button on the Qwiic bus, an old buzzer
alongside purely as the pull-up source (a bare Pico on a Qwiic cable is not a noknok host).
The module was SWD-flashed with stage-0 + stage-1 **v1.0.0**, no app, and the test then
staged a stage-1 **v1.0.1** — a build differing only in the version byte — over I²C.

| Step | Result |
|---|---|
| `0xB1 GET_VERSION` at `0x7E` | `[1, 1, 0, 0]` — proto 1, **v1.0.0** |
| `ERASE` + `WRITE_CHUNK` × 41 (2600 B) | ok |
| `0x06 VERIFY_STAGE1` | **READY** — CRC `0x92C8852C` matched, control block written |
| `BOOT` | `0x7E` went away, back after **284 ms** |
| `0xB1 GET_VERSION` | `[1, 1, 0, 1]` — **v1.0.1** |

**The changed version number is the proof.** Same bytes before and after would show
nothing; a version that went from 1.0.0 to 1.0.1 can only mean stage-0 did the copy.

Cross-checked from the SWD side immediately afterwards: stage-1 region byte-identical to the
v1.0.1 file (all 2600 bytes, and explicitly not v1.0.0), control block cleared, staging copy
untouched at `0x1000`, stage-0 untouched. The same event confirmed from two independent
directions.

Then the real LED Button app was installed on top (combined image) and the module booted
it — `0x7E` silent, full chain stage-0 → stage-1 v1.0.1 → application on real hardware.

**Two things the I²C session established that SWD could not:**

1. The `mtvec` alignment constraint (§7). This was the first time stage-1 took an interrupt.
2. The host must wait for `0x7E` to **disappear** before waiting for it to reappear. Right
   after `BOOT` the old stage-1 is still answering, so a plain "wait for bootloader" returns
   immediately with the *old* one and the test goes blind. `bench_stage1.py` has
   `wait_for_bootloader_gone()` for this. **The same hazard applies to the USB port** — same
   PID on both sides of the update — and is noted on the USB Confluence page.

**Also closed on 11 Sep:** the fast-erase probe re-run on JST-SH pigtail power (LinkE
target power off, board powered from the Pico) — **768 erases across four runs, zero
failures, zero hangs**. Combined with 10 Sep that is 2432 erases with exactly one anomaly,
in the very first four attempts ever, never repeated. Closed as an early bench transient;
stage-0's verify-and-retry covers the class regardless.

**All four power-loss windows proven, plus the two hardening-A attacks (11 Sep 2026, SWD-simulated, `run_swd_tests.sh` — 10/10):**

| Test | Simulates | Witness | Marker after |
|---|---|---|---|
| `stage_cut` | power cut mid-staging: half an image, no marker | A1 (old) | FF |
| `marker_nomagic` | marker write cut before the magic landed | A1 | FF |
| `marker_garbage` | marker write cut *after* the magic — descriptor all FF | A1 | **left set** (rejected as insane; documented) |
| `recover` | cut mid-copy: stage-1 half-written | A2 (new) | FF |
| `clear_cut` | cut during marker clear: copy complete, marker still set | A2 | FF |
| `desc_badcrc` | **hardening A, gate 1**: geometrically perfect descriptor, wrong descriptor CRC | A1 | **left set** (rejected) |
| `stage_corrupt` | **hardening A, gate 3**: perfect control block, one byte of the staged image flipped | A1 | **left set** (rejected) |

`marker_garbage` is the one that matters most: to a naive implementation it *looks* like a
pending update. Stage-0's descriptor sanity check rejects it and boots the old stage-1; the
marker stays set because stage-0 never clears one it did not act on (that would mean
unlocking flash on a normal boot). Harmless — stage-1 rewrites the block on the next real
update — and it means a corrupt marker can never drive an erase.

**Conductor integration — DONE (11 Sep 2026).** `ModuleFlasher` now carries `get_version()`,
`verify_stage1()`, `wait_for_bootloader_gone()` and the `flash_stage1()` orchestration; the
Conductor (`noknok.py`) exposes `bootloader_version(entry)` — the fleet discriminator — and
`stage1_update(entry, stage1_image, app_image=None)`, which replaces stage-1 and re-pushes the
app in one call. `brain-Pico/software/bench_conductor_stage1.py` is the product-shaped proof:
enumerate → read version → `stage1_update` with app restore → re-enumerate → read version.
On the bench: v1.0.1 → v1.0.2, app restored, module back at its runtime address, **9.1 s
end to end**. USB raises `NotImplementedError` until the CH32V203 port.

**Still to do:**
- The same exercise on the CH32V203 over USB.

## 8. Stage-1 — IMPLEMENTED (10 Sep 2026; hardened 11 Sep)

Source: `firmware/stage1/`. Relinked from `0x0000` to `0x0400`, derived from the
hardware-validated monolithic bootloader; **the application-flashing path is unchanged**.
Builds to **2984 B in the 4 KB region** (1112 B free; layout 2), 104 B RAM, as v1.2.0 — after
hardening C/D/E below (2600 B before those) and the v1.2.0 watchdog contract + layout byte
(+56 B over 1.1.0).

The design principle for the new commands: **a stage-1 update is the same transfer as an
application update.** Same `ERASE`, same `WRITE_CHUNK`, same staging area. Only the
closing command differs, so the host and the Conductor need almost no new machinery:

```
app update:      ERASE -> WRITE_CHUNK xN -> VERIFY(0x04)        -> BOOT
stage-1 update:  ERASE -> WRITE_CHUNK xN -> VERIFY_STAGE1(0x06) -> BOOT
```

| Command | Payload | Action |
|---|---|---|
| `0x06 VERIFY_STAGE1` | `[len(4 LE), crc32(4 LE)]` | CRC the staged image **and check its header** (hardening E); **only on both** write the control block (with its descriptor CRC, hardening A) that arms stage-0 |
| `0xB1 GET_VERSION` | — | next read returns `[BL_PROTOCOL_VERSION, major, minor, patch, layout]` — **5 bytes since v1.2.0**; byte 5 is the flash layout id from the image header (apps keep answering 4 bytes; a pre-1.2.0 stage-1 clocks out `0x00` there) |
| `0xB3 GET_UID` | — | **hardening D:** next read returns the 8-byte chip UID, same bytes/order as the app's enumeration reply |
| `0x05 BOOT` | — | **changed:** a pending stage-1 update now outranks booting the app — reset so stage-0 installs it |

New error codes on `READ_STATUS`: **7** = app unhealthy, stage-1 refused to boot it after
three failed attempts (hardening C — set at boot, cleared by the next command); **8** =
`VERIFY_STAGE1` refused the staged image because it is not a stage-1 for this layout.

Nothing is armed until the CRC matches, so a failed or interrupted transfer simply leaves
no marker and the module boots the existing stage-1 unchanged.

### Hardening C — the app boot-attempt counter

The most likely field failure is not a bootloader fault at all: an **application with a
valid CRC that crashes or hangs**. Stage-1 verified it at write time, so it boots it; it
dies; power-cycle → CRC still valid → boots it again. The only ways into flash mode were
`0xB0` (needs a working app) or a *bad* CRC. Dead until SWD.

Now `jump_to_app()` counts every jump in no-init RAM (`0x200007F8`, next to the handoff cell
in the 16 B every linker reserves). The app clears the cell the moment its I2C address is
assigned — enumeration completed, so I2C demonstrably works. A fresh app install (`VERIFY`)
resets it. On the boot path, a valid app whose counter has reached **3** is not booted:
stage-1 parks in flash mode with `last_error = 7`. Needs every app to run the IWDG so a hang
becomes a warm reset (LED Button v2.3, Buzzer v3.4.0, Knob v2.2.0, Display v0.4.0 do). A
`0xB0` reset never counts. Cold power-on randomises the cell → three fresh tries, which is
right for a transient.

**Refined by the first regression run (11 Sep 2026): the series only continues across
watchdog resets.** Stage-1 reads `RCC->RSTSCKR` at boot (and clears the flags with `RMVF`);
if the reset was *not* `IWDGRSTF` — power-on, software, SWD, NRST — the cell is zeroed first.
So "unhealthy" means exactly *three watchdog resets in a row*. The original "count every warm
boot" rule parked a perfectly good LED Button after three debugger reboots on the bench, and
would have done the same to a module re-plugged quickly with no Conductor around to assign
it. Costs 32 B (stage-1 was 2908 B in 3 KB at that point; now 2928 B in 4 KB after layout 2).

**Bench (`bad_app.c` — valid CRC, starts the IWDG, hangs):** witness `FF BA BA BA FF` — ran
on attempts 1, 2, 3, never a fourth; then `0x7E` answered `[3, 7]`.

### Watchdog contract — stage-1 arms the IWDG before the jump (v1.2.0, 12 Sep 2026)

Hardening C had a hole: it only counts **watchdog** resets, and the watchdog was started by
the *app*. An application that faults before it reaches its own `iwdg_init()` produces no
reset at all — it just hangs, silently, forever. That is exactly what the first real OTA run
did on 12 Sep 2026: a layout-2 buzzer image (linked at `0x1400`) was pushed onto a layout-1
buzzer, stage-1 wrote it at `0x1000`, the CRC passed (it is over the image bytes, not the
link address), the module jumped into wrongly-linked code and hung. Not parked. SWD clamp.

Now `jump_to_app()` arms the independent watchdog itself, immediately before handing over:
`PSCR = 4` (LSI ÷ 64), `RLDR = 0xFFF` → **~2.05 s nominal** (LSI is loosely specified, so
call it 1.3–3 s), then waits (bounded) for the PVU/RVU update flags to clear so the app's own
re-configuration is never dropped. The app inherits a *running* watchdog: its `iwdg_init()`
simply re-writes the same prescaler/reload (PSCR/RLDR stay writable after start) and its
first `iwdg_kick()` reloads it. If the app never gets that far, the chip resets with
`IWDGRSTF`, hardening C counts the strike, and after three the module parks at `0x7E` with
error 7 — where `rescue_parked_module()` finds it by UID and pushes the right image.
Today's clamp job becomes a reboot.

Why 2 s: the same value every layout-2 app already uses, so nothing about the apps' timing
changes; long enough for any legitimate boot (our apps reach their first kick in < 300 ms —
`iwdg_init()` is the first call after `SystemInit()`, then a startup cue of ≤ 200 ms, then
the main loop), short enough that a hung module is parked in ~6 s rather than minutes.
Longer would only delay the park; shorter starts eating into the startup cue.

Consequences, stated deliberately:

- **"Watchdog mandatory" is now enforced, not just required.** The IWDG cannot be stopped
  except by reset, so an application that never kicks it is parked after three strikes.
  Acceptable for layout 2 because every app was rebuilt with the watchdog block (LED Button
  2.4.0, Buzzer 3.5.0, Knob 2.3.0, Display 0.5.0). Recorded in the Ecosystem runbook §3.
- **The app must reach its first kick within the timeout.** A new app with a long blocking
  startup (a display animation, a self-test) must kick inside it — as the Display already
  does inside its backlight ramp.
- The watchdog is armed only on the jump-to-app path. Stage-1's own flash mode never arms
  it (flash mode does not kick), and a `0xB0` reset from the app stops it like any reset.
- Cost: the two v1.2.0 changes together are +56 B in stage-1 (2928 → 2984 B). Not a stage-0 change.

**Bench:** step 6 of `regress_all.sh` (`silent_app.c` — valid CRC, never touches the IWDG,
hangs): expected witness `FF 5A 5A 5A FF` and `0x7E` → `[3, 7]`. On a pre-1.2.0 stage-1 the
same image hangs with `0x7E` silent — the FAIL signature is the old behaviour.

### `0xB1` reports the layout (v1.2.0)

The bootloader's `GET_VERSION` reply grew from 4 to **5 bytes**:
`[proto, major, minor, patch, layout]`. Byte 5 is `stage1_hdr.layout` — the same word
`VERIFY_STAGE1` checks in a staged image — so the host reads *which flash map this module
runs* directly from the module, instead of keeping a "stage-1 1.0.x = layout 1, 1.1.x =
layout 2" table (the Conductor's `STAGE1_LAYOUTS`, now retirable). That table was the other
half of the 12 Sep incident: the host had to *infer* the layout, and inferred wrong.

Applications keep the 4-byte reply — only the bootloader's `0xB1` is 5 bytes, and there is
no ambiguity because the bootloader answers at `0x7E` and apps at their runtime address.
A host that reads 5 bytes from a pre-1.2.0 stage-1 receives `0x00` as byte 5 (the ISR pads
past `tx_len`), so `layout == 0` means "stage-1 too old to say", never a real layout.
`BL_PROTOCOL_VERSION` stays 1: the command set did not change, the reply is additive.

### Hardening D — `GET_UID`, and the Conductor's rescue path

A parked module does not enumerate, so nothing in the Conductor would ever see it — and
stage-1 cannot say what *type* of module it is. It can say its chip UID. `0xB3` returns the
same 8 bytes, in the same order, as the app's enumeration reply, so
`Conductor.rescue_parked_module()` (run **before** `enumerate()`) can probe `0x7E`, read the
UID, look it up in `noknok_state.json`, fetch that type's app, and push it. Doing this first
also keeps "one module in the bootloader at a time" true in practice.

**Found on the way:** the Conductor's `_save_state()` *replaced* the state file with the
current registry, so one enumeration with a module parked wiped its entry and the rescue
reported "unknown UID" for the very module it exists to rescue. Now merges. **Bench:** parked
by the bad app → enumerated with it parked (entry survived) → rescued by UID → back at
`0x09`.

### Hardening E — the image header

A 16-byte header `{"NKS1", base, layout, version}` sits at a **fixed** `+0x100` from the
image base (just past the 248-byte vector table), placed by `stage1.ld`. `VERIFY_STAGE1`
reads the same offset in the staged image and refuses (error 8) unless magic, base
(`0x0400`) and layout (1) all match. This closes the "wrong file" failure: the host computes
the CRC over whatever it was given, so a valid CRC alone proves nothing about *what* it is.
**Bench:** a 3000-byte valid-CRC blob with no header → error 8, control block untouched.

Linker gotcha caught on the first build: `AT>FLASH` on a section with an explicit VMA
continues the *load* address from the previous section, so the header landed at file
offset `0xF8` while the check reads `0x100`. Now an explicit `AT()` and a `LOADADDR`
`ASSERT`; the vector table is also asserted to stay under `0x100`.

**`0xB1` was chosen deliberately** to match the application's `GET_VERSION` (DEV-1) — same
command, same shape (4 bytes; the bootloader appends a 5th since v1.2.0, see below), so the
Conductor can ask anything on the bus for its version.
There is no ambiguity because the bootloader answers at `0x7E` and apps answer at their
runtime address. It also gives the fleet discriminator for free: **the old monolithic
bootloader does not implement `0xB1`, so silence means "old world" and a reply means
"stage-0/stage-1 module".** Stage-0's presence is implied — if stage-1 is running from
`0x0400`, something at `0x0000` jumped to it.

Two constants are kept deliberately distinct in stage-1, because conflating them is an
easy way to corrupt the control block later: `APP_REGION_LEN` (11136 B) bounds what an
*application* may occupy and stops short of the control block, while `APP_ERASE_LEN`
(11 KB) is what `ERASE` actually clears, so a fresh image wipes the marker and metadata
too.

## 9. CH32V203 (USB modules)

Same design, more headroom, and no dependency on the fast-erase result. The 1 KB stage-0
planned there already satisfies the vector-table alignment constraint found on the V003
(§7) — assume the QingKe V4 core has the same or a stricter requirement and keep stage-1
on a 1 KB boundary, with the same `ASSERT` in its linker script:

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
