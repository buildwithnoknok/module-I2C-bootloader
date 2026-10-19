#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# mk_full.py — build the full production-style image for the bench and the factory:
#   stage-0 @0x0000 + stage-1 @0x0400 + a real app @APP_BASE + app metadata.
# This is what regress_all.sh flashes to "restore the board", and (with --chip x035)
# the one SWD image for the noknok PicoHub's CH32X035 (DEV-124, factory note DEV-29).
# Layout constants must match the chip port (../../port/port_<chip>.h) and the apps'
# app.ld (ORIGIN). The image covers the whole user flash, so every page is written:
# nothing stale survives (flash programming only clears bits).
#
#   python3 mk_full.py <stage0.bin> <stage1.bin> <app.bin> <out.bin>              # CH32V003
#   python3 mk_full.py --chip x035 <stage0.bin> <stage1.bin> <app.bin> <out.bin>  # CH32X035
import struct, sys, zlib

CHIPS = {
    # chip: (flash size, app base, control block, metadata)
    'v003': (0x4000, 0x1400, 0x3F80, 0x3FC0),   # layout 2 (11 Sep 2026); app was 0x1000
    'x035': (0xF800, 0x1400, 0xF400, 0xF500),   # layout 3 (DEV-124), 62 KB, 256 B pages
}
STAGE1_BASE = 0x0400
META_MAGIC  = 0xB007C0DE

args = sys.argv[1:]
chip = 'v003'
if args[:1] == ['--chip']:
    chip, args = args[1], args[2:]
FLASH, APP_BASE, CTRL_OFF, META_OFF = CHIPS[chip]
s0, s1, app, out = args[:4]
s0  = open(s0,  'rb').read()
s1  = open(s1,  'rb').read()
app = open(app, 'rb').read()
assert len(s0) <= STAGE1_BASE,           'stage-0 overlaps stage-1'
assert len(s1) <= APP_BASE - STAGE1_BASE, 'stage-1 overlaps the app base'
assert len(app) <= CTRL_OFF - APP_BASE,   'app overlaps the control block'
img = bytearray(b'\xff' * FLASH)
img[0:len(s0)] = s0
img[STAGE1_BASE:STAGE1_BASE + len(s1)] = s1
img[APP_BASE:APP_BASE + len(app)] = app
crc = zlib.crc32(app) & 0xffffffff
img[META_OFF:META_OFF + 12] = struct.pack('<III', META_MAGIC, len(app), crc)
open(out, 'wb').write(img)
print('%s: stage-0 %d B  stage-1 %d B @0x%04X  app %d B @0x%04X  crc 0x%08X  meta @0x%04X -> %s (%d B)'
      % (chip, len(s0), len(s1), STAGE1_BASE, len(app), APP_BASE, crc, META_OFF, out, len(img)))
