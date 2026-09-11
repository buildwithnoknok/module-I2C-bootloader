#!/bin/sh
# DEV-31 I2C regression, end to end from the Pi: SWD-flash the starting state,
# run the self-update over I2C via the Pico, cross-check over SWD, restore.
set -e
S1=/home/noknok/dev/ch32fun/noknok_stage1
M=/home/noknok/dev/ch32fun/minichlink/minichlink
P=/home/noknok/dev/pico

echo '=== 1. SWD: flash stage-0 + stage-1 v1.0.0 (final source), no app ==='
cd $S1
python3 mk_bench.py 0x400 s1_0400_v100.bin bench_0400_start.bin
$M -w bench_0400_start.bin flash -b >/dev/null 2>&1 && echo 'flashed'
sleep 1; $M -b >/dev/null 2>&1; sleep 1

echo
echo '=== 2. Pico: push current bench_stage1.py + v1.0.1 payload ==='
cd $P
./pico.py put bench_stage1.py
./pico.py put noknok_stage1_v101.bin

echo
echo '=== 3. Pico: run the self-update over I2C ==='
./pico.py run bench_stage1.py

echo
echo '=== 4. SWD cross-check ==='
cd $S1
LEN=$(stat -c %s s1_0400_v101.bin)
$M -r + 0x08000400 $LEN 2>/dev/null | grep '^080' | cut -d: -f2 | tr -d ' \n' > chip.hex
od -A n -t x1 -v s1_0400_v101.bin | tr -d ' \n' > v101.hex
if cmp -s chip.hex v101.hex; then echo "stage-1 region == v1.0.1  (all $LEN bytes)"; else echo 'MISMATCH'; exit 1; fi
echo -n 'control block: '; $M -r + 0x08003F80 4 2>/dev/null | grep '^080' | cut -d: -f2
echo -n 'staging @0x1000: '; $M -r + 0x08001000 4 2>/dev/null | grep '^080' | cut -d: -f2

echo
echo '=== 5. SWD: restore stage-0 + stage-1 v1.0.1 + LED Button app ==='
python3 mk_full.py
$M -w full_stage_ledbutton.bin flash -b >/dev/null 2>&1 && echo 'restored'
sleep 1; $M -b >/dev/null 2>&1
echo 'done'
