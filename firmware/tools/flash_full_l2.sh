#!/bin/sh
# flash_full_l2.sh <combined_16k.bin> [expected_uniid1] [-3|-t]
#
# SWD-flash a combined layout-2 image (built by build_full_l2.py) onto a CH32V003
# module and verify it byte-for-byte. Whole-chip erase, write, read back, compare,
# reboot -- the same proven flow as restore_buzzer_l2.sh, generalised.
#
#   <combined_16k.bin>  the 16 KB image from build_full_l2.py
#   [expected_uniid1]   optional safety gate: minichlink's R32_ESIG_UNIID1 value
#                       (little-endian, e.g. 55ddabcd) -- refuses if the clamped
#                       chip is not that board. Omit to flash whatever is clamped.
#   [-3|-t]             LinkE target power. Default -3 (LinkE powers a clamp-only
#                       board). Use -t for a board powered from its Qwiic pigtail.
#
# minichlink does NOT verify writes, so the read-back compare here is mandatory.
set -e
IMG="$1"
WANT1="$2"
PWR="${3:--3}"
M="$HOME/dev/ch32fun/minichlink/minichlink"
RB=/tmp/full_l2_readback.bin

[ -f "$IMG" ] || { echo "no image: $IMG"; exit 2; }

echo "== probe ($PWR)"
$M $PWR -i 2>&1 | tee /tmp/full_l2_probe.txt | grep -E 'Detected|Part UUID|Read protection|UNIID[12]'
grep -q "Read protection: disabled" /tmp/full_l2_probe.txt \
    || { echo "REFUSE: read protection ON -- run 'minichlink -p' first"; exit 2; }
if [ -n "$WANT1" ]; then
    grep -q "R32_ESIG_UNIID1: $WANT1" /tmp/full_l2_probe.txt \
        || { echo "REFUSE: clamped chip UNIID1 != $WANT1 (wrong board)"; exit 2; }
fi

echo "== erase + write + read back (one session)"
$M $PWR -E -w "$IMG" flash -r $RB 0x08000000 16384 2>&1 | grep -v '^$' | tail -6
cmp "$IMG" $RB && echo "READBACK OK (16384 B identical)" \
    || { echo "READBACK MISMATCH"; exit 1; }

echo "== reboot"
$M $PWR -b 2>&1 | tail -1
echo "done"
