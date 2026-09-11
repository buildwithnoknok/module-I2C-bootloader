#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# mk_full.py — build the full production-style image for the bench:
#   stage-0 @0x0000 + stage-1 @0x0400 + a real app @APP_BASE + app metadata @0x3FC0.
# This is what regress_all.sh flashes to "restore the board". Layout constants
# must match noknok_stage1.c (APP_BASE) and the apps' app.ld (ORIGIN).
#
#   python3 mk_full.py <stage0.bin> <stage1.bin> <app.bin> <out.bin>
import struct, sys, zlib

STAGE1_BASE = 0x0400
APP_BASE    = 0x1400            # layout 2 (11 Sep 2026); was 0x1000
META_OFF    = 0x3FC0
META_MAGIC  = 0xB007C0DE

s0, s1, app, out = sys.argv[1:5]
s0  = open(s0,  'rb').read()
s1  = open(s1,  'rb').read()
app = open(app, 'rb').read()
assert len(s0) <= STAGE1_BASE,           'stage-0 overlaps stage-1'
assert len(s1) <= APP_BASE - STAGE1_BASE, 'stage-1 overlaps the app base'
assert len(app) <= 0x3F80 - APP_BASE,     'app overlaps the control block'
img = bytearray(b'\xff' * 0x4000)
img[0:len(s0)] = s0
img[STAGE1_BASE:STAGE1_BASE + len(s1)] = s1
img[APP_BASE:APP_BASE + len(app)] = app
crc = zlib.crc32(app) & 0xffffffff
img[META_OFF:META_OFF + 12] = struct.pack('<III', META_MAGIC, len(app), crc)
open(out, 'wb').write(img)
print('stage-0 %d B  stage-1 %d B @0x%04X  app %d B @0x%04X  crc 0x%08X -> %s'
      % (len(s0), len(s1), STAGE1_BASE, len(app), APP_BASE, crc, out))
