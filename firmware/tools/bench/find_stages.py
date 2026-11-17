# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# find_stages.py FULL_IMAGE.bin STAGE0_DIR STAGE1_DIR - host: which stage-0 / stage-1 .bin is inside a known-good full image (byte compare, rest of region erased).
import os, glob, sys
img = open(sys.argv[1], 'rb').read()
s0_region = img[0x0000:0x0400]
s1_region = img[0x0400:0x1400]

def match(region, path):
    d = open(path, 'rb').read()
    if len(d) > len(region):
        return None
    if region[:len(d)] != d:
        return False
    # rest of the region must be erased
    return all(x == 0xFF for x in region[len(d):])

cands0 = glob.glob(os.path.join(sys.argv[2], '*.bin'))
cands1 = glob.glob(os.path.join(sys.argv[3], '*.bin'))

print("== stage-0 candidates ==")
for p in sorted(cands0):
    print("  %-55s len=%-5d match=%s" % (os.path.basename(p), os.path.getsize(p), match(s0_region, p)))
print("== stage-1 candidates ==")
for p in sorted(cands1):
    print("  %-55s len=%-5d match=%s" % (os.path.basename(p), os.path.getsize(p), match(s1_region, p)))

# also show the actual non-FF extent of each region in the image
def extent(region):
    e = len(region)
    while e > 0 and region[e-1] == 0xFF:
        e -= 1
    return e
print("image stage-0 non-FF bytes: %d" % extent(s0_region))
print("image stage-1 non-FF bytes: %d" % extent(s1_region))
