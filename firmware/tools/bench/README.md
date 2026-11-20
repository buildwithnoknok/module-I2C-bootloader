<!-- SPDX-FileCopyrightText: 2026 noknok (Christopher Houben) -->
<!-- SPDX-License-Identifier: MIT -->
# firmware/tools/bench - bench helpers for the CH32V003 bootloader

Taken into git by DEV-77 (17 Nov 2026); until then they lived only on a bench host. They complement
`../build_full_l2.py` + `../flash_full_l2.sh` (full layout-2 image over SWD) and the regression suite in
`../../stage0/test/`. Bench host = a Linux box with a WCH-LinkE and `minichlink`; Pico scripts run with
brain-Pico `tools/pico.py run`.

| File | Runs on | What it does |
|---|---|---|
| `restore_buzzer_l2.sh` | host + LinkE | UID-gated SWD restore of the bench Buzzer to layout 2 (erase, write, read back + compare, reboot). Edit `WANT1/WANT2` + `IMG` for another board. |
| `verify_buzzer_l2.py` | Pico | S-tier smoke after a restore: enumerate, list modules, read the Buzzer's stage-1 version (`0xB1`). |
| `bench_stage1_buzzer.py` | Pico | Install a new stage-1 on the bench Buzzer through the running stage-1 (real OTA path), then verify. |
| `probe_parked.py` | Pico | Talk to a module parked in its bootloader at `0x7E` (GET_VERSION, state). |
| `probe_getid.py` | Pico | `0xB4` GET_ID x100 at `0x7E` (parked, vs `0xB3`) or at every enumerated module (vs its enumeration UID): CRC-8, UID, stable type. PASS/FAIL per address (DEV-21 / DEV-22). |
| `bench_dev22_type.py` | Pico | DEV-22: stage-1 1.3.0 identity on a V003 module with a DEV-21 app - 0xB1/0xB4 at 0x7E, type kept across ERASE and two power cuts (PicoHub CYCLE_RAIL), X035-header app refused (error 9), headerless app = type 0. Bench 20 Nov 2026: 10/10 PASS. |
| `bench_dev22_s1update.py` | Pico | DEV-22: stage-1 self-update over I2C 1.2.0 -> 1.3.0 -> 1.3.1 -> 1.3.0 with app re-push; type survives a 1.3.x self-update. 8/8 PASS. |
| `bench_dev22_x035.py` | Pico | DEV-22/21: PicoHub X035 stage-1 1.0.0 -> 1.1.0 and app 0.4.0 over I2C, 0xB4 = 0x07 at the app and at 0x7E, then restores the released parts. 8/8 PASS. |
| `bench_dev21_apps.py` | Pico | DEV-21: OTA the 0xB4 apps (Knob, LED Button, Display) and check 0xB1 + 0xB4 x100; `MODE = "old"` restores the released apps. |
| `inspect_l2.py FULL.bin` | host | Dump a 16 KB layout-2 image: region starts, every `B007C0DE` metadata block, region ends. |
| `find_stages.py FULL.bin S0_DIR S1_DIR` | host | Which stage-0 / stage-1 `.bin` is inside a known-good full image. |
