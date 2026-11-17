#!/bin/sh
# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# restore_buzzer_l2.sh -- SWD-restore the bench buzzer (UID cdab90e274bcd14a) to
# layout 2: stage-0 + stage-1 v1.1.x + buzzer app 3.5.0 @0x1400 + metadata.
# Bench config: LinkE target power OFF (-t), clamp GND+SWIO only, board powered
# from the JST-SH pigtail (Pico Qwiic 3V3). Refuses if the clamped chip is not
# the buzzer. Whole-chip erase, write, read back + byte compare, reboot.
set -e
M=$HOME/dev/ch32fun/minichlink/minichlink
IMG=$HOME/dev/ch32fun/noknok_buzzer/full_buzzer_l2.bin
RB=/tmp/buzzer_readback.bin
WANT1=e290abcd   # UNIID1 of cdab90e274bcd14a (little-endian words)
WANT2=4ad1bc74
cd $HOME/dev/ch32fun
echo "== probe"
$M -t -i 2>&1 | tee /tmp/buzzer_probe.txt | grep -E 'Detected|Part UUID|Read protection|UNIID[12]'
grep -q "R32_ESIG_UNIID1: $WANT1" /tmp/buzzer_probe.txt || { echo "REFUSE: clamped chip is not the buzzer ($WANT1/$WANT2)"; exit 2; }
grep -q "Read protection: disabled" /tmp/buzzer_probe.txt || { echo "REFUSE: read protection ON -- run minichlink -p first"; exit 2; }
echo "== erase + write + read back (one session)"
$M -t -E -w $IMG flash -r $RB 0x08000000 16384 2>&1 | grep -v '^$' | tail -6
cmp $IMG $RB && echo "READBACK OK (16384 B identical)" || { echo "READBACK MISMATCH"; exit 1; }
echo "== reboot"
$M -t -b 2>&1 | tail -1
sleep 4
echo "done -- now run on the Pi: cd ~/dev/pico && python3 pico.py run verify_buzzer_l2.py"
