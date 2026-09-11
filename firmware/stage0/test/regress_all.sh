#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# regress_all.sh — THE regression for the stage-0 / stage-1 / app / Conductor
# chain (DEV-31). One command, ~4 minutes, PASS/FAIL table at the end.
#
# Run this before: any stage-1 release, any change to stage-0 (should be
# never), any app change touching boot/enumeration/watchdog, any Conductor
# change touching flashing or state, and before DEV-29 programs the batch.
#
# What each step proves, its preconditions and expected result: TESTS.md
# (next to this file). Bench setup: Confluence "Firmware Bench — How to Build
# and Drive One". In short: LinkE on the module's SWD pads (target power OFF),
# Pico on the Pi with the LED Button on its I2C bus plus a pull-up source
# (a legacy buzzer is fine — it also serves as the "other module" in step 6),
# and these Pico files pushed (brain-Pico/software, via ./pico.py put):
#   noknok.py module_flasher.py bench_stage1.py bench_conductor_stage1.py
#   bench_wrongfile.py bench_state_merge.py keyboard_firmware.bin
# The stage-1 payloads are built and pushed by this script.
#
# ORDER MATTERS — each step leaves the board in a state the next one relies on:
#   1 swd       ends: board restored (full image, stage-1 v1.0.1, app)   [~60 s]
#   2 i2c       flashes its own start image (v1.0.0); ends restored       [~30 s]
#   3 conductor needs a running app + a DIFFERENT stage-1 (pushes v1.0.2) [~15 s]
#   4 wrongfile needs a running app; ends restored                        [~15 s]
#   5 badapp    flashes a hanging app; ends PARKED at 0x7E (error 7)      [~25 s]
#   6 merge     needs a parked module + its UID in noknok_state.json
#               (learned by 3/4); rescues it; ends running                [~15 s]
# Exit 0 = all pass, 1 = a step failed (logs in /tmp/*.log), 2 = preflight/build.

T=/home/noknok/dev/ch32fun/noknok_stage0/test
S1=/home/noknok/dev/ch32fun/noknok_stage1
P=/home/noknok/dev/pico
M=/home/noknok/dev/ch32fun/minichlink/minichlink

# lines of Conductor enumeration chatter to strip from the per-step summaries
QC='Enumerating\|already assigned\|^Done\|No new modules\|^  Noknok\|^  noknok'

pass=""; fail=""
run() {  # run <name> <sh -c command>
  name=$1; shift
  echo; echo "################ $name ################"
  if sh -c "$*"; then pass="$pass $name"; else fail="$fail $name"; fi
}

# -- preflight ---------------------------------------------------------------
echo "=== preflight ==="
lsusb | grep -q 1a86:8010 || { echo "FAIL: WCH-LinkE not on USB"; exit 2; }
ls /dev/serial/by-id/usb-Raspberry_Pi_Pico*-if00 >/dev/null 2>&1 || { echo "FAIL: Pico not on USB"; exit 2; }
$M -i 2>&1 | grep -q 'Detected CH32V003' || { echo "FAIL: no CH32V003 on SWD"; exit 2; }
LS=$(cd $P && ./pico.py ls / 2>/dev/null)
for f in noknok.py module_flasher.py bench_stage1.py bench_conductor_stage1.py \
         bench_wrongfile.py bench_state_merge.py keyboard_firmware.bin; do
  echo "$LS" | grep -q "$f" || { echo "FAIL: $f not on the Pico"; exit 2; }
done
echo "LinkE, Pico (+ bench files), chip: ok"

# -- build everything from SOURCE so we test what is committed, not leftovers --
# Three stage-1 versions: v1.0.0 = start state for the I2C test, v1.0.1 = its
# payload and the version in the "full" restore image, v1.0.2 = the Conductor
# test's payload (it must differ from what is installed, or nothing is proven).
echo "=== build ==="
build_s1() {  # build_s1 <patch> <out.bin>   (run inside $S1)
  make clean >/dev/null 2>&1
  make build EXTRA_CFLAGS=-DS1_VERSION_PATCH=$1 >/tmp/build_s1.log 2>&1 || { tail -5 /tmp/build_s1.log; return 1; }
  cp noknok_stage1.bin "$2"
}
( cd $T/.. && make clean >/dev/null 2>&1 && make build >/tmp/build_s0.log 2>&1 \
    && ls -l noknok_stage0.bin | awk '{print "stage-0:", $5, "B"}' ) \
  || { tail -5 /tmp/build_s0.log; echo "FAIL: stage-0 build"; exit 2; }
( cd $S1 && build_s1 0 s1_0400_v100.bin && build_s1 1 s1_0400_v101.bin && build_s1 2 s1_0400_v102.bin \
    && cp s1_0400_v100.bin noknok_stage1.bin \
    && ls -l s1_0400_v100.bin | awk '{print "stage-1:", $5, "B (built as v1.0.0 / v1.0.1 / v1.0.2)"}' \
    && python3 mk_full.py ) || { echo "FAIL: stage-1 build"; exit 2; }
cp $S1/s1_0400_v101.bin $P/noknok_stage1_v101.bin
cp $S1/s1_0400_v102.bin $P/noknok_stage1_new.bin
( cd $P && ./pico.py put noknok_stage1_v101.bin >/dev/null && ./pico.py put noknok_stage1_new.bin >/dev/null ) \
  || { echo "FAIL: could not push payloads to the Pico"; exit 2; }
echo "payloads on the Pico: v1.0.1 (i2c step), v1.0.2 (conductor step)"

# -- 1. stage-0 SWD suite (10 images: jump, update, all power-loss windows, attacks) --
run swd "cd $T && python3 build_test_images.py >/dev/null 2>&1 \
  && sh run_swd_tests.sh 2>&1 | tee /tmp/swd.log | grep -E 'PASS|FAIL|restored' \
  && grep -q 'ALL PASS' /tmp/swd.log"

# -- 2. stage-1 self-update over I2C, SWD cross-checked ------------------------
run i2c "sh $T/i2c_regress.sh 2>&1 | tee /tmp/i2c.log \
  | grep -E 'v1\.0\.|region ==|control block|MISMATCH|restored|FAIL' \
  && grep -q 'ALL PASS' /tmp/i2c.log && grep -q 'region ==' /tmp/i2c.log"

# -- 3. the product path: Conductor.stage1_update() with app restore ----------
run conductor "cd $P && ./pico.py run bench_conductor_stage1.py 2>&1 | tee /tmp/cond.log \
  | grep -v '$QC' | grep -E 'before|after|restored|ALL PASS|FAIL' \
  && grep -q 'ALL PASS' /tmp/cond.log"

# -- 4. hardening E: a non-stage-1 image is refused ---------------------------
run wrongfile "cd $P && ./pico.py run bench_wrongfile.py 2>&1 | tee /tmp/wf.log \
  | grep -v '$QC' | grep -E 'refused|ALL PASS|FAIL' \
  && grep -q 'ALL PASS' /tmp/wf.log"

# -- 5. hardening C: a valid-CRC app that hangs is parked after 3 tries -------
run badapp "cd $T && sh run_badapp_test.sh 2>&1 | tee /tmp/bad.log \
  | grep -E '^080|status|UNHEALTHY|unexpected' \
  && grep -q 'ff ba ba ba ff' /tmp/bad.log && grep -q 'APP UNHEALTHY' /tmp/bad.log"

# -- 6. hardening D: state survives a parked module; rescue by UID -------------
run merge "cd $P && ./pico.py run bench_state_merge.py 2>&1 | tee /tmp/merge.log \
  | grep -v '$QC' | grep -E 'SURVIVED|reflashed|LED Buttons now|ALL PASS|FAIL' \
  && grep -q 'ALL PASS' /tmp/merge.log"

# -- summary -------------------------------------------------------------------
echo; echo "=================== REGRESSION SUMMARY ==================="
for n in swd i2c conductor wrongfile badapp merge; do
  case " $pass " in *" $n "*) printf '  %-10s PASS\n' $n;; *) printf '  %-10s FAIL\n' $n;; esac
done
echo "=========================================================="
if [ -z "$fail" ]; then echo "ALL PASS"; exit 0; else echo "FAILED:$fail  (see /tmp/<step>.log)"; exit 1; fi
