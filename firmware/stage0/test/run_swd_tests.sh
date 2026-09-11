#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# Run every stage-0 SWD test image and check what each leaves behind.
# For each: flash, wait, read the witness (which stage-1 ran) and the control
# block's first word (is an update still marked pending?).
#
# Expected:
#   jump            A1   marker FF        no update -> booted installed A1
#   update          A2   marker FF        update applied, marker cleared
#   recover         A2   marker FF        interrupted copy redone, marker cleared
#   stage_cut       A1   marker FF        partial staging, no marker -> A1 untouched
#   marker_nomagic  A1   marker FF        magic never landed -> not pending
#   marker_garbage  A1   marker 6E6B5530  magic OK but descriptor insane -> rejected,
#                                         marker deliberately LEFT SET (documented)
#   clear_cut       A2   marker FF        copy was complete, marker set -> redone, cleared
#   chain           A3   marker FF        stage-0 -> real stage-1 -> app
#
# Run from the Pi in the test dir after build_test_images.py.

cd "$(dirname "$0")"
M=../../minichlink/minichlink
fail=0

check() {
  name=$1; want_w=$2; want_m=$3
  $M -w test_$name.bin flash -b >/dev/null 2>&1
  sleep 2
  w=$($M -r + 0x08003C00 1 2>/dev/null | grep '^080' | cut -d: -f2 | tr -d ' ')
  m=$($M -r + 0x08003F80 4 2>/dev/null | grep '^080' | cut -d: -f2 | tr -d ' ')
  # control block word 0 as little-endian hex
  m_le=$(echo $m | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
  if [ "$w" = "$want_w" ] && [ "$m_le" = "$want_m" ]; then r=PASS; else r=FAIL; fail=1; fi
  printf '%-16s witness=%s  marker=%s   %s\n' "$name" "$w" "$m_le" "$r"
}

check jump           a1 ffffffff
check update         a2 ffffffff
check recover        a2 ffffffff
check stage_cut      a1 ffffffff
check marker_nomagic a1 ffffffff
check marker_garbage a1 6e6b5530
check clear_cut      a2 ffffffff
[ -f test_chain.bin ] && check chain a3 ffffffff

[ $fail = 0 ] && echo 'ALL PASS' || echo 'FAILURES'

# Restore the board. The last test leaves a FAKE app installed that does not
# enumerate — walking away like that cost a confused Conductor test on 11 Sep.
# If a full production image is available next to the stage-1 build, flash it.
RESTORE=../../noknok_stage1/full_stage_ledbutton.bin
if [ -f "$RESTORE" ]; then
  $M -w "$RESTORE" flash -b >/dev/null 2>&1 && echo "board restored from $RESTORE"
else
  echo "WARNING: board left with the test_chain FAKE app — restore it before I2C work"
fi
$M -b >/dev/null 2>&1
exit $fail
