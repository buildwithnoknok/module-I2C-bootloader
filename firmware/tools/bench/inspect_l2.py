# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# inspect_l2.py FULL_IMAGE.bin - host: dump a 16 KB layout-2 image: first bytes of stage-0/1/app, every B007C0DE metadata block, where each region ends.
import struct
import sys
p = sys.argv[1]
b = open(p, 'rb').read()
print("size", len(b))
for off in (0x0000, 0x0400, 0x1400):
    print("0x%04X:" % off, b[off:off+16].hex())
# find every B007C0DE (metadata magic) occurrence
magic = struct.pack('<I', 0xB007C0DE)
i = 0
while True:
    j = b.find(magic, i)
    if j < 0:
        break
    length, crc = struct.unpack('<II', b[j+4:j+12])
    print("magic @ 0x%04X  len=%d  crc=0x%08X" % (j, length, crc))
    i = j + 1
# any other non-FF near the top of each 4K page tail
print("non-FF rows in 0x3F00-0x4000:")
for off in range(0x3F00, 0x4000, 16):
    row = b[off:off+16]
    if any(x != 0xFF for x in row):
        print("  0x%04X:" % off, row.hex())
# where does stage-0 end (first FF run after 0)?
def region_end(start):
    e = start
    while e < len(b) and b[e] != 0xFF:
        e += 1
    return e
print("stage0 non-FF ends ~0x%04X" % region_end(0))
print("stage1 non-FF ends ~0x%04X" % region_end(0x400))
print("app    non-FF ends ~0x%04X" % region_end(0x1400))
