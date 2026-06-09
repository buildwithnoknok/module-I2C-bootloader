# noknok I²C Module Bootloader

Shared **CH32V003 bootloader** that lets any noknok I²C module be re-flashed
**over the I²C bus** — no SWDIO cable required. One codebase, SWD-flashed once per
module at manufacture; all later application updates happen wirelessly via the Pico.

This is the firmware foundation for **PoC v2 Step 4** (wireless module OTA). Full
design rationale lives in Confluence → *Software Development* → "I²C Module
Bootloader — Design & Process (PoC v2 Step 4)".

> **Status:** written and compiling (2132 B in a 4 KB region). Not yet flashed or
> bench-tested on hardware.

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

## Compatibility

Generic across all noknok **CH32V003** I²C modules (Buzzer, Knob, LED Button).
USB-C modules (CH32V203) are out of scope and will need a separate bootloader.

Each module's application must be **relinked at the `0x1000` offset** and reserve
the top 16 bytes of RAM for the handoff cell. See the design doc for details.
