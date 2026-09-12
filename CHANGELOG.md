<!--
SPDX-License-Identifier: MIT
Copyright (c) noknok
-->

# Changelog — noknok I²C module bootloader

Stage-0 is frozen at manufacture; stage-1 is field-updatable (DEV-31). Versions below are
what `0xB1 GET_VERSION` at `0x7E` reports. There is no rollback: every stage-1 release is
gated by `firmware/stage0/test/regress_all.sh`.

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
