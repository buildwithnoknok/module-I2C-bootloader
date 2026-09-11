#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# Hardening C on the bench: flash a valid-CRC app that hangs, let the watchdog
# reset it three times, then check (a) over I2C that stage-1 has parked and
# says why and (b) over SWD how many times the app actually ran.
#
# Order matters: the I2C check comes FIRST. Any SWD access (even a read) halts
# the core and on resume the chip reboots, which starts a fresh attempt series
# (stage-1 only continues the series across watchdog resets). So after the
# witness read we reboot deliberately, wait for the bad app to be parked
# again, and confirm it — the module is then PARKED at 0x7E on purpose
# (regress_all.sh step 6 needs it).
cd "$(dirname "$0")"
M=../../minichlink/minichlink
P=/home/noknok/dev/pico
cp badapp_status.py $P/ 2>/dev/null

echo '--- flash test_badapp.bin (stage-0 + real stage-1 + hanging app + valid metadata) ---'
$M -w test_badapp.bin flash -b >/dev/null 2>&1
echo '--- waiting 9 s for three ~2 s watchdog cycles ---'
sleep 9
echo '--- over I2C: parked at 0x7E with error 7? ---'
( cd $P && ./pico.py run badapp_status.py )

echo '--- witness bytes [0..4]: expect FF BA BA BA FF (ran on attempts 1,2,3; never a 4th) ---'
$M -r + 0x08003C00 5 2>/dev/null | grep '^080'

echo '--- reboot after the SWD read; the bad app runs its three tries again and is parked ---'
$M -b >/dev/null 2>&1
sleep 9
echo -n 'final state: '
( cd $P && ./pico.py run badapp_status.py )
