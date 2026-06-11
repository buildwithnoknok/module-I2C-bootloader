# noknok I²C Module Bootloader

Shared **CH32V003 bootloader** that lets any noknok I²C module be re-flashed
**over the I²C bus** — no SWDIO cable required. One codebase, SWD-flashed once per
module at manufacture; all later application updates happen wirelessly via the Pico.

This is the firmware foundation for **PoC v2 Step 4** (wireless module OTA). Full
design rationale lives in Confluence → *Software Development* → "I²C Module
Bootloader — Design & Process (PoC v2 Step 4)".

> **Status:** **hardware-validated (Jun 2026).** The full I²C OTA loop is proven on
> the LED Button board — blank-board first flash, running-app `0xB0` round-trip, and
> power-loss-mid-flash recovery all pass, with the buzzer and knob daisy-chained on the
> same bus. Bootloader is 2132 B in the 4 KB region. The buzzer and knob applications
> are relinked for the bootloader as well.

## Flash map (16 KB)

| Region | Address | Size | Written by | Notes |
|--------|---------|------|-----------|-------|
| Bootloader | `0x0000_0000` | 4 KB | SWD (once) | Runs first on every reset. Immutable in the field. |
| Application | `0x0000_1000` | 12 KB | I²C OTA | The module's real firmware, linked at the `0x1000` offset. |
| Metadata | `0x0000_3FC0` | 64 B | I²C OTA | Validity marker: `{magic, app_length, app_crc32}`. |

The flash controller addresses flash via the `0x0800_0000` alias; execution/reset
uses the `0x0000_0000` alias.

## Boot decision (every reset)

1. Handoff magic set in no-init RAM (`0x200007F0`)? → **flash mode** (app asked for update).
2. Else `CRC32(app) == metadata.crc32`? → **jump to app** (`0x1000`).
3. Else → **flash mode** (blank/corrupt chip → safe recovery).

A half-flashed or blank module always lands safely in the bootloader. SWD remains
the unbrickable hardware fallback at all times.

## I²C protocol (flash mode, address `0x7E`)

| Command | Dir | Payload | Action |
|---------|-----|---------|--------|
| `0x01` ERASE | write | — | Erase the app region + metadata (12 × 1 KB). |
| `0x02` WRITE_CHUNK | write | `[offHi, offLo, 64 B]` | Fast-program one 64-byte page. |
| `0x03` READ_STATUS | read | → `[state, last_error]` | `state`: 0 IDLE / 1 BUSY / 2 READY / 3 ERROR. |
| `0x04` VERIFY | write | `[len(4 LE), crc32(4 LE)]` | CRC-check, then write the validity marker. |
| `0x05` BOOT | write | — | If a valid app is present, reset into it. |

A running app enters the bootloader when the Pico sends it command `0xB0`
(ENTER_BOOTLOADER): the app writes magic `0x6E6B4231` to `0x200007F0` and resets.

**CRC32** = standard zlib (poly `0xEDB88320`, init/final `0xFFFFFFFF`) — matches
Python `binascii.crc32` on the Pico. The Pico pads the final 64-byte page with
`0xFF`; CRC is computed over the true app length.

## Build & flash

Requires [cnlohr/ch32fun](https://github.com/cnlohr/ch32fun) checked out next to
this project (the Makefile includes `../ch32fun/ch32fun.mk`). Build from
`firmware/src/`:

```sh
make build      # → noknok_bootloader.bin  (2132 B)
make flash      # SWD-flash via WCH-LinkE + minichlink (one-time, per module)
```

The custom 4 KB layout is set by `firmware/src/bootloader.ld`.

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

Run once per blank board, or to recover a module whose **bootloader** got corrupted:

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
USB-C modules (CH32V203) are out of scope and will need a separate bootloader.

Each module's application must be **relinked at the `0x1000` offset** and reserve
the top 16 bytes of RAM for the handoff cell. See the design doc for details.

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
