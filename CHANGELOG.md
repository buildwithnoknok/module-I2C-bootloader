<!--
SPDX-License-Identifier: MIT
Copyright (c) noknok
-->

# Changelog — noknok I²C module bootloader

Stage-0 is frozen at manufacture; stage-1 is field-updatable (DEV-31). Versions below are
what `0xB1 GET_VERSION` at `0x7E` reports. There is no rollback: every stage-1 release is
gated by `firmware/stage0/test/regress_all.sh`.

## Stage-1 V003 1.3.0 + X035 1.1.0 - 2026-11-20 (DEV-22) - NOT RELEASED

Source only on branch `dev-22-stage1-v3-id`; `firmware/bin/` and `index*.json` unchanged until
the bench runs and the L-tier regression pass (release build on the bench toolchain).

- **`0xB4 GET_ID` (ADR-005)** at `0x7E`: next read = `[UID x8][TYPE][CRC8]`, the same 10 bytes
  as an app's 0x7F enumeration reply; latched in the ISR at STOP like `0xB1`/`0xB3`.
- **Module type in the metadata page** (word 3). VERIFY writes it from the app header (V003:
  `{"NKA1","V003",2,version,type}` at app base + 0x100, DEV-21) or the fixed type (X035:
  0x07). ERASE keeps it: it programs only the type word back into the blank metadata page,
  the magic stays blank, a half-loaded image never boots. Survives a stage-1 self-update too.
- **V003 app header check (tolerant):** no header = installs as before with type 0; a header
  for another chip or layout = error 9 (no metadata written).
- **V003 Flashing Interface V3:** in flash mode PD1 is left alone for ~2 s (plain SWD connect
  works), then SDI is disabled (`AFIO->PCFR1` SWCFG = 0b100, volatile) and the LED shows
  double blink (waiting), 1 Hz (transfer since ERASE) or 5 Hz (error / parked). The normal
  boot path (valid app) does NOT touch SDI or PD1. Test builds: `-DBL_V3_OFF` keeps the old
  LED and never disables SDI (the regression scripts read flash over SWD in flash mode).
- `mk_full.py` writes the type word into the metadata (from the app header; X035 0x07).
- Size (agent-hq gcc 14.2, ch32fun 95030fd, not the release toolchain): V003 3516 B of 4096
  (+520 B on that toolchain's 2996 B baseline), X035 3680 B of 4096 (+280 B). Breaking: no
  (layouts, `0xB1`, `0xB3` and the update protocol unchanged). Not tested on hardware yet.

## Test tooling - 2026-11-17 (DEV-77)

- DEV-77 - bench helpers that lived only on a bench host are now in `firmware/tools/bench/` (Buzzer layout-2 SWD restore + verify, stage-1 install on the bench Buzzer, parked-module probe, layout-2 image inspectors). Tools only, no firmware change. Breaking: no.

## Test tooling - 2026-11-10 (DEV-231)

- `regress_all.sh`: preflight accepts the compiled `noknok.mpy` / `module_flasher.mpy` (no 250 KB source on a product brain); the restore image uses the release stage-1 (patch 0), not the test build; `badapp_status.py` reads its I2C pins from `settings.toml`. Found by the 9 Nov 2026 run on a PicoHub rig (4 steps MemoryError, 2 steps wrong pins). Dry-checked only (shell syntax, preflight cases, pin fallback with stubs); the suite was not re-run. No firmware change, no version change.

## Chip ports - 2026-10-19 (DEV-124)

- The bootloader is now one shared core plus a small port header per chip (`firmware/port/`).
  **CH32V003:** no change: `noknok_stage0.bin` and `noknok_stage1.bin` (1.2.0) rebuild
  byte-identical, so no version moves. **CH32X035** (the power manager of the noknok PicoHub,
  MOD-0011): first build, flash layout 3. `mk_full.py` takes `--chip x035`. Breaking: no.

## Stage-1 (CH32X035)

### 1.0.0 - 2026-10-19
- DEV-124 - First CH32X035 stage-1 (3392 B / 4 KB), same protocol and hardening as the V003
  stage-1 1.2.0. Layout 3; header at base + 0x200 with target `X035`; VERIFY refuses an app
  without the X035 app header (new error 9); 256 B pages programmed from four 64 B chunks; reset
  flags left for the app on the boot path. Bench: updates, self-update to a 1.0.1 test build and
  back, foreign-image refusal, interrupted update, 7 power cuts during updates. Breaking: no.

## Stage-0 (CH32X035)

### 1 KB frozen build - 2026-10-19 (688 B)
- DEV-124 - Same source as the V003 stage-0; control block `0xF400`, 256 B pages, attempt cell
  `0x20004FF4`. Frozen once the PicoHub production batch is programmed.

## Stage-1

### 1.2.0 — 12 Sep 2026
- **Arms the IWDG (~2 s) before jumping to the application.** An app that dies before its
  own `iwdg_init()` (wrongly-linked image, early fault) is now watchdog-reset, counted by
  hardening C and parked at `0x7E` with error 7 after three strikes — instead of hanging
  silently until SWD (the 12 Sep 2026 layout-mismatch incident). The IWDG cannot be
  stopped: every application must kick it, first kick within ~2 s of the jump.
- **`0xB1 GET_VERSION` reply is now 5 bytes:** `[proto, major, minor, patch, layout]`.
  Byte 5 = flash layout id from the image header (`2`). Apps still answer 4 bytes. A
  pre-1.2.0 stage-1 read for 5 bytes yields `0x00` = "did not say".
- Size 2984 B / 4 KB (1112 B free). Regression gains step 6 `silentapp` (an app that never
  arms a watchdog must still end parked).
- Breaking for hosts that assumed a 4-byte bootloader reply; authorised (whole fleet on the
  bench, nothing shipped).

### 1.1.0 — 11 Sep 2026
- **Layout 2:** stage-1 region 3 KB → 4 KB, application base `0x1000` → `0x1400`.
  Header layout id 1 → 2; `VERIFY_STAGE1` refuses a cross-layout image (error 8).
  Apps relinked: LED Button 2.4.0, Buzzer 3.5.0, Knob 2.3.0, Display 0.5.0. Stage-0
  untouched (it reads `app_base` from the control block).

### 1.0.0 — 10/11 Sep 2026
- First field-updatable bootloader: monolithic bootloader relinked to `0x0400`, plus
  `VERIFY_STAGE1` (0x06), `GET_VERSION` (0xB1), `GET_UID` (0xB3), the app boot-attempt
  counter (hardening C, watchdog-cause rule), the image header (hardening E). Layout 1
  (stage-1 3 KB, app `0x1000`).

## Stage-0

### 1 KB frozen build — 11 Sep 2026 (704 B)
- Descriptor CRC + geometry + staged-image CRC gates before any erase (hardening A);
  install failure → retry by reset, up to 4 boots (hardening B); verify-and-retry per page.
  Hardcodes only the stage-1 base (`0x0400`) and the control block (`0x3F80`).

## Legacy (`firmware/src/`, monolithic 4 KB bootloader, app at `0x1000`)
- Jun 2026, hardware-validated. What modules ran before DEV-31. Does not answer `0xB1`.
