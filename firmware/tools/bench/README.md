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
| `inspect_l2.py FULL.bin` | host | Dump a 16 KB layout-2 image: region starts, every `B007C0DE` metadata block, region ends. |
| `find_stages.py FULL.bin S0_DIR S1_DIR` | host | Which stage-0 / stage-1 `.bin` is inside a known-good full image. |
