# noknok I²C Module Bootloader

Shared **CH32V003 bootloader** that lets any noknok I²C module be re-flashed
**over the I²C bus** — no SWDIO cable required. One codebase, SWD-flashed once per
module at manufacture; all later application updates happen wirelessly via the Pico.

This is the firmware foundation for **PoC v2 Step 4** (wireless module OTA). Full
design rationale lives in Confluence → *Software Development* → "I²C Module
Bootloader — Design & Process (PoC v2 Step 4)".

> **Status:** **hardware-validated.** The full I²C OTA loop is proven on the LED Button
> board (Jun 2026) — blank-board first flash, running-app `0xB0` round-trip, and
> power-loss-mid-flash recovery all pass, with the buzzer and knob daisy-chained on the
> same bus. The buzzer and knob applications are relinked for the bootloader as well.
>
> **Stage-0 / stage-1 split (Sep 2026, DEV-31):** built and SWD-bench-validated — 4/4
> tests, including recovery from a deliberately half-written stage-1. Stage-0 is 500 B
> in a frozen 1 KB reservation; stage-1 is 2600 B in 3 KB. **Validated over I²C on
> 11 Sep 2026** — a real stage-1 self-update v1.0.0 → v1.0.1 over the bus, confirmed from
> both the I²C and SWD sides. `firmware/src/` (the monolithic bootloader) is still what every
> existing module runs until they are re-flashed.

## Repo layout

| Path | What | Status |
|------|------|--------|
| `firmware/stage0/` | Frozen stage-0 — applies stage-1 updates, then jumps to stage-1. Never changes after manufacture. | New (DEV-31) |
| `firmware/stage1/` | The real bootloader, relinked to `0x0400`. I²C flashing + self-update. | New (DEV-31) |
| `firmware/src/` | The original monolithic 4 KB bootloader. | **Legacy** — what shipped modules run until re-flashed; retire once they are |
| `firmware/stage0/test/` | Stage-0 bench harness + test-image builder. | — |
| `firmware/test/fast_erase_probe/` | Probe proving CH32V003 64-byte erase granularity. | — |
| `docs/stage0-design.md` | Full stage-0 / stage-1 design spec. | — |

## Flash map (16 KB)

| Region | Address | Size | Written by | Notes |
|--------|---------|------|-----------|-------|
| **Stage-0** | `0x0000_0000` | 1 KB | SWD (once) | Runs first on every reset. **Frozen forever.** Measured 500 B. |
| **Stage-1** | `0x0000_0400` | 3 KB | stage-0, from staging | The real bootloader. Field-updatable. Measured 2600 B. **Base must be 1 KB aligned** (mtvec). |
| Application | `0x0000_1000` | ~11.9 KB | I²C OTA | The module's real firmware, linked at the `0x1000` offset. |
| **Control block** | `0x0000_3F80` | 64 B | stage-1 (set) / stage-0 (clear) | Update marker + staging descriptor + `app_base`. |
| Metadata | `0x0000_3FC0` | 64 B | I²C OTA | Validity marker: `{magic, app_length, app_crc32}`. |

The split fits inside the **existing 4 KB bootloader reservation**, so the application
base stays at `0x1000` and **module applications need no relinking** — the split is
invisible to them.

The flash controller addresses flash via the `0x0800_0000` alias; execution/reset
uses the `0x0000_0000` alias.

## Boot decision (every reset)

Two levels. Stage-0 decides only whether to install a pending bootloader update;
everything about the application stays stage-1's job.

**Stage-0**

1. Control-block marker set and staging descriptor sane? → erase stage-1, copy
   staging → stage-1 (verify + retry per page), clear the marker, **reset**.
2. Else stage-1's first word isn't `0xFFFFFFFF`? → **jump to stage-1** (`0x0400`).
3. Else → no stage-1 ever programmed: halt, SWD recovery. (Factory error only.)

**Stage-1** — behaviour unchanged

1. Handoff magic set in no-init RAM (`0x200007F0`)? → **flash mode** (app asked for update).
2. Else `CRC32(app) == metadata.crc32`? → **jump to app** (`0x1000`).
3. Else → **flash mode** (blank/corrupt chip → safe recovery).

A half-flashed or blank module always lands safely in the bootloader. **Stage-0 never
touches SDI**, so SWD remains the unbrickable hardware fallback at all times — including
in stage-0's halt state.

## I²C protocol (flash mode, address `0x7E`)

| Command | Dir | Payload | Action |
|---------|-----|---------|--------|
| `0x01` ERASE | write | — | Erase the app region + metadata (12 × 1 KB). |
| `0x02` WRITE_CHUNK | write | `[offHi, offLo, 64 B]` | Fast-program one 64-byte page. |
| `0x03` READ_STATUS | read | → `[state, last_error]` | `state`: 0 IDLE / 1 BUSY / 2 READY / 3 ERROR. |
| `0x04` VERIFY | write | `[len(4 LE), crc32(4 LE)]` | CRC-check, then write the validity marker. |
| `0x05` BOOT | write | — | Boot the app — or, if a stage-1 update is pending, reset so stage-0 installs it. |
| `0x06` VERIFY_STAGE1 | write | `[len(4 LE), crc32(4 LE)]` | **stage-1 only.** CRC-check the staged stage-1; on match, arm stage-0. |
| `0xB1` GET_VERSION | write, then read | → `[proto, major, minor, patch]` | **stage-1 only.** Bootloader version. |

**A stage-1 update is the same transfer as an application update** — same `ERASE`, same
`WRITE_CHUNK`, same staging area (the app region). Only the closing command differs:

```
app update:      ERASE → WRITE_CHUNK ×N → VERIFY(0x04)        → BOOT
stage-1 update:  ERASE → WRITE_CHUNK ×N → VERIFY_STAGE1(0x06) → BOOT
```

Nothing is armed until the CRC matches, so a failed or interrupted transfer leaves no
marker and the module boots the existing stage-1 unchanged. A bootloader update always
destroys the application (the staging area *is* the app region), so the host re-pushes
the app straight afterwards.

`0xB1` matches the application's `GET_VERSION` so the Conductor can ask anything on the
bus for its version — no ambiguity, since the bootloader answers at `0x7E` and apps at
their runtime address. The legacy monolithic bootloader doesn't implement it, so
**silence means "old world" and a reply means "stage-0/stage-1 module"**.

A running app enters the bootloader when the Pico sends it command `0xB0`
(ENTER_BOOTLOADER): the app writes magic `0x6E6B4231` to `0x200007F0` and resets.

**CRC32** = standard zlib (poly `0xEDB88320`, init/final `0xFFFFFFFF`) — matches
Python `binascii.crc32` on the Pico. The Pico pads the final 64-byte page with
`0xFF`; CRC is computed over the true app length.

## Build & flash

Requires [cnlohr/ch32fun](https://github.com/cnlohr/ch32fun) checked out next to
this project.

```sh
cd firmware/stage0 && make build   # → noknok_stage0.bin  (500 B / 1 KB)
cd firmware/stage1 && make build   # → noknok_stage1.bin  (2600 B / 3 KB)
```

Layouts are set by `stage0/stage0.ld` (`ORIGIN 0x0000`, 1 KB) and
`stage1/stage1.ld` (`ORIGIN 0x0400`, 3072 B). Both linker scripts `ASSERT` their
start address, so a mismatched pair fails the build rather than producing a stage-0
that jumps into nothing.

**Stage-0 does not use `ch32fun.mk`.** It links only its own `start.S` plus its C
file, using `ch32fun.h` for register definitions only. Linking `ch32fun.c` cost
248 B — a 38-entry interrupt vector table and a general-purpose reset handler, in
code that never enables an interrupt. Dropping it took stage-0 from 936 B to 500 B.
Stage-1 *does* use `ch32fun.mk`; it needs the vector table for the I²C handlers.

To flash a module, build one combined image rather than flashing the stages
separately — see [Recovery & SWD flashing](#recovery--swd-flashing) below.
`firmware/stage0/test/build_test_images.py` builds exactly this kind of image.

The legacy monolithic bootloader still builds from `firmware/src/`
(`make build` → `noknok_bootloader.bin`, 2132 B, 4 KB layout via `bootloader.ld`).

## Recovery & SWD flashing

The **5-pin SWIO header** (`GND, SWIO, RST, VCC`) is the unbrickable hardware
backstop. It talks to the RISC-V debug module directly — independent of flash
contents — so it works even when flash is blank or corrupt. I²C OTA can **never**
permanently brick a module: the flasher refuses to write below `0x1000` (the
bootloader's own region) and only writes the validity marker after a full,
CRC-verified flash, so a failed or interrupted OTA simply leaves the module waiting
safely in the bootloader at `0x7E`.

Flash-controller addresses use the `0x0800_0000` alias.

### 1. Install / restore the bootloader (the normal recovery)

Run once per blank board, or to recover a module whose **bootloader** got corrupted.

With the stage-0 / stage-1 split, flash **one combined image** containing stage-0 at
`0x0000` and stage-1 at `0x0400` — flashing the two separately would whole-erase the
chip between steps and wipe the first one:

```sh
minichlink -w combined.bin flash -b
```

Legacy monolithic bootloader (what shipped modules run):

```sh
cd firmware/src
make flash        # SWD-flash the bootloader at 0x0800_0000 via WCH-LinkE + minichlink
```

minichlink whole-erases the chip first, so this also clears any application +
metadata. On the next boot the bootloader finds no valid app → waits in flash mode at
`0x7E`. From there, flash the application over I²C from the Pico
(`module_flasher.py` in `brain-Pico`). **This is the standard way to bring up a blank
board and to recover from almost any failure.**

### 2. SWD-flash an application directly (skip the Pico)

The application is linked at the `0x1000` offset, so it must be written at flash
address **`0x0800_1000`** — *not* `0x0800_0000` (that would overwrite the
bootloader). The bootloader will also only jump to the app once a valid **metadata
marker** exists at `0x0800_3FC0`, which a bare SWD app-write does not create.

→ In practice, **don't**: use the I²C flasher, which writes the app *and* the
metadata in one pass. If you only need to get a module running on the bench without
the Pico, use option 3 instead.

### 3. Bench / last-resort: run an app WITHOUT the bootloader

For bench debugging or a guaranteed-working unit, build the application as a normal
**full-flash image (linked at `0x0000`)** and SWD-flash it at `0x0800_0000`:

- Build the module's pre-bootloader (standalone) firmware — i.e. with the stock
  ch32fun linker, *without* the `LINKER_SCRIPT := app.ld` line — and `make flash`.
- minichlink whole-erases and writes at `0x0800_0000`; the chip runs the app
  directly, no bootloader, no OTA.
- To return the module to the OTA system, repeat **option 1** (re-flash the
  bootloader), then flash the app over I²C.

### Notes

- **SWIO is shared with the status LED (PD1).** The WCH-LinkE drives SWIO during
  connect/reset; bench-validate that SWD still flashes reliably with the LED fitted on
  the module (the LED is currently only on the flashing fixture). *(Open item.)*
- The bootloader never erases or writes its own region, so option 1 is always
  available as long as the SWIO header is reachable.

## Compatibility

Generic across all noknok **CH32V003** I²C modules (Buzzer, Knob, LED Button).
USB-C modules (CH32V203) use the separate
[module-USB-bootloader](https://github.com/buildwithnoknok/module-USB-bootloader),
which gets the same stage-0 / stage-1 treatment with its own layout.

Each module's application must be **relinked at the `0x1000` offset** and reserve
the top 16 bytes of RAM for the handoff cell. See the design doc for details.

**The stage-0 split changes nothing for module applications.** The app base is still
`0x1000` and the bootloader still occupies the same 4 KB reservation — the split is
internal to it. Existing relinked apps run unmodified under stage-1.

## Flashing a factory / read-protected board

Brand-new CH32V003 chips often ship with **read protection enabled** (RDPR), which
silently blocks flashing. `minichlink -i` will read the chip ID but show
`Read protection: enabled`, and writes fail with "nothing connected"-style errors.

Disable protection first (this also mass-erases the chip - fine for a blank board),
then flash:

    minichlink -p      # disable read protection + mass-erase
    make flash         # now writes normally

Bench tip: when hand-holding wires to the 5 SWD pads, contact is flaky (3V3 is the
usual dropout). Run `make flash` in a short retry loop and it catches the moment
contact is good. Pin grabbers directly on the MCU legs are more reliable than the pads.

---

## License

- Code: MIT — see [LICENSE](LICENSE).

---

## Safety & Liability

noknok hardware is an electronic device and a DIY/maker kit. You assemble, modify, flash, power, and operate it at your own risk, and it is provided as is, without warranty. See the full notice: [License, Safety & Liability](https://buildwithnoknok.github.io/safety-and-license/).
