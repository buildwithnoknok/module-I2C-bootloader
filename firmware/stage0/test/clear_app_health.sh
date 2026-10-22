#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# Zero the app boot-attempt counter (no-init RAM 0x200007F8) over SWD.
#
# Why the bench needs this: stage-1 counts every warm boot of the app and only
# the app clears the count, when a Conductor assigns it an address. The SWD
# rig reboots the module several times per regression with no Conductor
# running, so a perfectly good app reaches three counted boots and stage-1
# parks it (error 7) — which is correct behaviour, but not what a "restore the
# board" step wants. In the field a power cycle does what this script does.
#
# Usage: sh clear_app_health.sh   (chip is left halted; follow with minichlink -b)
M=/home/noknok/dev/ch32fun/minichlink/minichlink
printf '\0\0\0\0' > /tmp/zero4
$M -w /tmp/zero4 0x200007F8 >/dev/null 2>&1
$M -r + 0x200007F8 4 2>/dev/null | grep -q '^200007f8: 00 00 00 00' || { echo "clear_app_health: cell NOT cleared"; exit 1; }
