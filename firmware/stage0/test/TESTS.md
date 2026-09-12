# DEV-31 regression catalogue — stage-0 / stage-1 / app / Conductor

**Run it:** on the Pi, `sh /home/noknok/dev/ch32fun/noknok_stage0/test/regress_all.sh`
(about 4 minutes, prints a PASS/FAIL table). Everything below is what that one command runs,
in order, and what a FAIL in each step means. The scripts are the source of truth — this file
explains them; it never replaces them.

**When to run it — mandatory before:**

| Change | Why |
|---|---|
| any stage-1 release (`firmware/bin/noknok_stage1.bin`) | stage-1 is field-updatable but has **no rollback** — the regression is the gate |
| any change to stage-0 | should never happen after the first batch; if it does, all 10 SWD images must still pass |
| an app change touching boot, enumeration, `SystemInit()`, the watchdog or the linker script | the app is a party to the boot contract (health clear, reserved RAM) |
| a Conductor change touching `module_flasher.py`, `_save_state()`, `stage1_update()`, `rescue_parked_module()` | steps 3, 4 and 7 are the Conductor's proof |
| programming a production batch (DEV-29) | last look before the frozen stage-0 goes into 100+ boards |

Everything else (an app's feature code, Pico product scripts) does not need it.

**Bench needed:** the reference bench from Confluence *"Firmware Bench — How to Build and Drive
One"* — WCH-LinkE on the LED Button's SWD pads (target power OFF, GND + SWIO), Pico on the Pi
with the LED Button on its I2C bus, a pull-up source on the bus (a legacy buzzer works and
doubles as the "other module" for step 7), the bench files listed at the top of `regress_all.sh`
on the Pico. Read protection on the LED Button must be off.

The runner **builds stage-0, stage-1 and the LED Button app from source** first (three stage-1
versions PATCH 0 / 1 / 2 of whatever `S1_VERSION_*` says, e.g. 1.2.0 / 1.2.1 / 1.2.2; the
full restore image is assembled by `mk_full.py`), so it tests what is checked out, not
whatever binaries were lying around. Layout constants (app base `0x1400`) live in
`mk_full.py`, `build_test_images.py`, `fake_app.ld`, `fake_stage1.ld` — keep them in step
with `noknok_stage1.c` and the apps' `app.ld`.

**The runner reboots the module many times over SWD with no Conductor running.** That is
only harmless because stage-1 continues the app boot-attempt series (hardening C) **across
watchdog resets only** — any other reset cause (power-on, software, SWD, NRST) starts a fresh
series. The first run of this regression (11 Sep 2026) found the original "count every warm
boot" rule parking a perfectly good app after three debugger reboots; the same false positive
would have hit a module re-plugged quickly with no Conductor around. If `conductor`/`wrongfile`
ever fail with "no LED Button" right after a passing `swd`/`i2c`, suspect that rule first.

---

## Step 1 — `swd` · stage-0 alone, driven over SWD (`run_swd_tests.sh`)

Ten images built by `build_test_images.py`. Each = stage-0 + a *fake* stage-1 (writes a
witness word to `0x3C00` and halts, so we can tell which stage-1 ran) + staging area + control
block set up to represent one situation. After flashing, the witness and the control-block
marker are read back and compared.

| Image | Situation | Proves | Expected |
|---|---|---|---|
| `jump` | no marker, stage-1 A1 installed | stage-0 does nothing and jumps | witness A1, marker FF |
| `update` | marker set, A2 staged, good CRCs | the normal install path | witness A2, marker cleared |
| `recover` | marker set, stage-1 **half written** (power cut mid-copy) | DEV-31 core acceptance: recovery from a torn stage-1 | witness A2, marker cleared |
| `stage_cut` | power cut **mid-staging**: partial image, marker never written | nothing pending → old stage-1 untouched | witness A1, marker FF |
| `marker_nomagic` | power cut while writing the marker, **before** the magic landed | not pending → old stage-1 | witness A1, marker FF |
| `marker_garbage` | power cut while writing the marker, **after** the magic: descriptor insane | gate 2 (geometry) refuses; no erase; marker deliberately left set | witness A1, marker `6E6B5530` |
| `clear_cut` | stage-1 fully installed, marker **not yet cleared** (power cut before the last erase) | re-running the install is idempotent | witness A2, marker cleared |
| `desc_badcrc` | perfect geometry, wrong descriptor CRC (hardening A) | gate 1 refuses **before** any erase; marker left set | witness A1, marker `6E6B5530` |
| `stage_corrupt` | one flipped byte in the staged image, descriptor CRC valid (hardening A) | gate 3 refuses; marker left set | witness A1, marker `6E6B5530` |
| `chain` | stage-0 + **real** stage-1 + a fake app (writes A3) | the whole boot chain stage-0 → stage-1 → app | witness A3, marker FF |

The runner then flashes the **full image** (stage-0 + stage-1 PATCH+1 + LED Button app) so the
board is back to a product state. A "marker left set" after a refusal is by design: the next boot re-checks, and stage-1 can rewrite it. A FAIL here is a stage-0 or control-block regression — the
worst kind, because stage-0 cannot be fixed in the field. Hardening B (retry-by-reset) is code-
reviewed only; a flash failure cannot be provoked on demand.

## Step 2 — `i2c` · stage-1 updates itself over the bus (`i2c_regress.sh` → `bench_stage1.py`)

SWD-flashes stage-0 + the base stage-1 build with no app (module sits at `0x7E`). The Pico then
runs the raw `module_flasher` sequence: `GET_VERSION`, transfer the PATCH+1 build into staging,
`VERIFY_STAGE1`, `BOOT`, wait for `0x7E` to disappear *and reappear*, `GET_VERSION` → PATCH+1.
Then the Pi reads the stage-1 region back over SWD and compares it byte-for-byte with the
pushed file, and checks the control block was cleared and the staging area erased. Restores
the full image afterwards.

Proves: the real protocol end to end (`0x06`, `0xB1`, the header at `+0x100`, the disappear/
reappear wait), and that what lands in flash is exactly the file. A FAIL with `MISMATCH` is a
stage-0 copy bug; a FAIL earlier is a stage-1 protocol bug or a `module_flasher.py` regression.

## Step 3 — `conductor` · the product path (`bench_conductor_stage1.py`)

Starts from a running app. `Conductor.bootloader_version()` (drops the module into its
bootloader, asks `0xB1`, boots it back) → `Conductor.stage1_update(entry, PATCH+2 build, app_image)`
→ `bootloader_version()` again → PATCH+2, and the app is enumerated again at its old address.

Proves: what the noknok app will actually call, including the app-restore step (a stage-1
update overwrites the app region, so the Conductor must reflash the app). A FAIL here with
steps 1–2 passing is in `noknok.py`.

## Step 4 — `wrongfile` · hardening E (`bench_wrongfile.py`)

Sends a valid-CRC blob that is **not** a stage-1 image (the LED Button app, truncated to fit
the size gate) through `stage1_update()`. Expected: refused with **error 8** (`not a stage-1
image`), control block untouched, module boots its old stage-1, app still runs.

Proves: the 16-byte image header at `+0x100` is checked before anything is written. A FAIL is
either a stage-1 `do_verify_stage1()` regression or the linker putting the header somewhere
else (`stage1.ld` has an ASSERT for this — check the build log first).

## Step 5 — `badapp` · hardening C (`run_badapp_test.sh` → `badapp_status.py`)

SWD-flashes stage-0 + real stage-1 + `bad_app.c` (valid metadata and CRC, starts the IWDG,
records a witness byte, then hangs) and waits ~9 s. Expected: `0x7E` answering `[3, 7]` = flash
mode, error 7 *app unhealthy*, and the witness `FF BA BA BA FF` (the app ran on attempts 1, 2, 3
and never a 4th). The I2C check runs before the SWD read: any SWD access reboots the chip on
resume, which starts a fresh series — so the script reboots once more afterwards, waits, and
confirms the module is parked again.

Proves: a valid-but-broken app is parked after three watchdog resets instead of being dead
until SWD — the biggest field win of the hardening round. **Leaves the module parked on
purpose** — step 7 needs it (step 6 overwrites it with its own image first). A FAIL with a witness of `FF BA BA BA BA` means the counter is
not being read/incremented across resets (no-init RAM cell `0x200007F8`).

## Step 6 — `silentapp` · stage-1 v1.2.0 watchdog contract (`run_badapp_test.sh silentapp`)

Same harness as step 5 with `silent_app.c`: valid metadata and CRC, records a witness byte
(`0x5A`), then hangs **without ever touching the IWDG**. Expected: exactly as step 5 —
`0x7E` answering `[3, 7]` and the witness `FF 5A 5A 5A FF`.

Proves: stage-1 arms the independent watchdog (~2 s) *before* jumping to the application, so
an app that dies before its own `iwdg_init()` — a wrongly-linked image, an early fault — is
still reset, counted and parked instead of hanging silently. This is the direct test of the
12 Sep 2026 incident (layout-2 image on a layout-1 module: hung, needed SWD). **On a stage-1
older than 1.2.0 this step FAILS with `0x7E silent`** — that is the module hung, exactly the
old behaviour. **Leaves the module parked on purpose** — step 7 needs it.

## Step 7 — `merge` · hardening D (`bench_state_merge.py`)

With the LED Button parked from step 6: `enumerate()` (only the buzzer answers) → check
`noknok_state.json` still holds the LED Button's UID → type entry → `rescue_parked_module()`
(probes `0x7E`, reads the UID with `0xB3`, looks the type up, reflashes the app, boots) →
`enumerate()` finds the LED Button again.

Proves: the Conductor never forgets a module that is temporarily in its bootloader (the bug
found on 11 Sep 2026: `_save_state()` replaced the file), and the UID-based rescue works
end to end. A FAIL at "forgotten" is `_save_state()`; a FAIL at "did not reflash" is
`rescue_parked_module()` or `GET_UID` in stage-1.

**Board state after a full pass:** stage-0 + the base stage-1 build (the `badapp`/`silentapp` images embed it) + LED Button app, enumerated. Run `regress_all.sh` again any time — every step sets up its own start state.

---

## Not in the runner (manual, run when relevant)

| Script | What | When |
|---|---|---|
| `brain-Pico/software/bench_rescue.py` | rescue path alone, on a module you parked by hand | debugging rescue |
| `brain-Pico/software/bench_push_app.py` | push an app to the bench buzzer through `update_module()` and read `GET_VERSION` back | after changing a buzzer/knob/display app's boot block — those boards are not on the SWD rig |
| `firmware/test/fast_erase_probe/` | the 64-byte fast-erase characterisation (2432 erases) | only if the silicon or the flash routine changes |
| USB / CH32V203 side | not covered yet — needs the PIO-USB rig on the Pico | DEV-31 open item |

## Adding a test

Add the image to `build_test_images.py` + a `check` line in `run_swd_tests.sh` (stage-0 behaviour),
or a `bench_*.py` in brain-Pico that prints `ALL PASS` on success and raises on failure (bus /
Conductor behaviour), then a `run` line in `regress_all.sh` **in an order that respects the
board state** the neighbours expect, and a row here.
