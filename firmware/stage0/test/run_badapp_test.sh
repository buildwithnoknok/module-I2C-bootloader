#!/bin/sh
# Hardening C on the bench: flash a valid-CRC app that hangs, let the watchdog
# reset it three times, then check (a) over SWD how many times it ran and
# (b) over I2C that stage-1 has parked and says why.
cd "$(dirname "$0")"
M=../../minichlink/minichlink
P=/home/noknok/dev/pico
echo '--- flash test_badapp.bin (stage-0 + real stage-1 + hanging app + valid metadata) ---'
$M -w test_badapp.bin flash -b >/dev/null 2>&1
echo '--- waiting 9 s for three ~2 s watchdog cycles ---'
sleep 9
echo '--- witness bytes [0..4]: expect FF BA BA BA FF (ran on attempts 1,2,3; never a 4th) ---'
$M -r + 0x08003C00 5 2>/dev/null | grep '^080'
$M -b >/dev/null 2>&1
sleep 1
echo '--- over I2C: parked at 0x7E with error 7? ---'
cp badapp_status.py $P/ 2>/dev/null
cd $P && ./pico.py run badapp_status.py
