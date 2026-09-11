#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# mk_bench.py — stage-0 + stage-1 bench image with NO app and no marker, so the
# module sits in stage-1 flash mode at 0x7E. Start state for the I2C self-update test.
#
#   python3 mk_bench.py <stage0.bin> <stage1.bin> <out.bin>
import sys
STAGE1_BASE = 0x0400
s0 = open(sys.argv[1], 'rb').read()
s1 = open(sys.argv[2], 'rb').read()
assert len(s0) <= STAGE1_BASE, 'stage-0 overlaps stage-1 base'
img = bytearray(b'\xff' * 0x4000)
img[0:len(s0)] = s0
img[STAGE1_BASE:STAGE1_BASE + len(s1)] = s1
open(sys.argv[3], 'wb').write(img)
print('stage-0 %d B @0x0000   stage-1 %d B @0x%04X   -> %s' % (len(s0), len(s1), STAGE1_BASE, sys.argv[3]))
