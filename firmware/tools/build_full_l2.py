#!/usr/bin/env python3
"""
build_full_l2.py -- assemble a single SWD-flashable 16 KB image that restores a
noknok CH32V003 module to full LAYOUT-2 production state in one write. This is
the SWD equivalent of what the Pico writes over I2C OTA.

Flash map (layout 2, since 11 Sep 2026):
    0x0000  stage-0 bootloader   (<= 1 KB, ends 0x0400)
    0x0400  stage-1 bootloader   (<= 4 KB, ends 0x1400)
    0x1400  application          (<= 10 KB, ends 0x3C00, below the metadata page)
    0x3FC0  metadata {magic=0xB007C0DE, app_len, zlib.crc32(app)}  (little-endian)
    everything else = 0xFF (erased)

stage-1 validates the app against this metadata before jumping to 0x1400, so the
app_len and CRC MUST describe exactly the app bytes placed at 0x1400.

Flash the result with (clamp-only board -> -3; Qwiic-pigtail board -> -t):
    minichlink -3 -E -w <out.bin> flash -r /tmp/rb.bin 0x08000000 16384
then `cmp <out.bin> /tmp/rb.bin` and reboot with `minichlink -3 -b`.

NOT for layout 1 (legacy app@0x1000) -- that map is retired.

Usage:
    build_full_l2.py --stage0 s0.bin --stage1 s1.bin --app app.bin --out full.bin
                     [--expect-crc 01216df3] [--verify-stages known_good_l2.bin]
"""
import argparse, struct, zlib

STAGE0_ORIGIN = 0x0000
STAGE1_ORIGIN = 0x0400
APP_ORIGIN    = 0x1400
META_ORIGIN   = 0x3FC0
IMG_SIZE      = 0x4000          # 16 KB total flash
META_MAGIC    = 0xB007C0DE


def build(stage0, stage1, app):
    """Return (16 KB image bytearray, app_crc32). Raises on any region overflow."""
    if len(stage0) > STAGE1_ORIGIN - STAGE0_ORIGIN:
        raise SystemExit("stage-0 too big: %d B > %d B region"
                         % (len(stage0), STAGE1_ORIGIN - STAGE0_ORIGIN))
    if len(stage1) > APP_ORIGIN - STAGE1_ORIGIN:
        raise SystemExit("stage-1 too big: %d B > %d B region"
                         % (len(stage1), APP_ORIGIN - STAGE1_ORIGIN))
    if len(app) > META_ORIGIN - APP_ORIGIN:
        raise SystemExit("app too big: %d B > %d B region"
                         % (len(app), META_ORIGIN - APP_ORIGIN))

    img = bytearray(b"\xff" * IMG_SIZE)
    img[STAGE0_ORIGIN:STAGE0_ORIGIN + len(stage0)] = stage0
    img[STAGE1_ORIGIN:STAGE1_ORIGIN + len(stage1)] = stage1
    img[APP_ORIGIN:APP_ORIGIN + len(app)] = app

    crc = zlib.crc32(app) & 0xFFFFFFFF
    img[META_ORIGIN:META_ORIGIN + 12] = struct.pack("<III", META_MAGIC, len(app), crc)
    return img, crc


def main():
    ap = argparse.ArgumentParser(description="Build a combined layout-2 SWD image.")
    ap.add_argument("--stage0", required=True, help="stage-0 bootloader .bin")
    ap.add_argument("--stage1", required=True, help="stage-1 bootloader .bin")
    ap.add_argument("--app",    required=True, help="application .bin (linked @ 0x1400)")
    ap.add_argument("--out",    required=True, help="output combined .bin")
    ap.add_argument("--expect-crc", help="assert the app's crc32 (hex, e.g. 01216df3)")
    ap.add_argument("--verify-stages", metavar="KNOWN_GOOD",
                    help="assert bytes [0,0x1400) match this known-good image "
                         "(proves stage-0+stage-1 are the canonical bench pair)")
    a = ap.parse_args()

    s0  = open(a.stage0, "rb").read()
    s1  = open(a.stage1, "rb").read()
    app = open(a.app, "rb").read()
    img, crc = build(s0, s1, app)

    if a.expect_crc:
        want = a.expect_crc.lower().replace("0x", "")
        if ("%08x" % crc) != want:
            raise SystemExit("CRC MISMATCH: built 0x%08X, expected %s" % (crc, want))

    if a.verify_stages:
        good = open(a.verify_stages, "rb").read()
        if img[:APP_ORIGIN] != good[:APP_ORIGIN]:
            raise SystemExit("STAGE MISMATCH: bytes [0,0x1400) differ from %s -- "
                             "wrong stage-0/stage-1 bins" % a.verify_stages)
        print("stage verify OK (bytes [0,0x1400) match %s)" % a.verify_stages)

    open(a.out, "wb").write(img)
    print("stage0=%d  stage1=%d  app=%d  app_crc=0x%08X  out=%s  size=%d"
          % (len(s0), len(s1), len(app), crc, a.out, len(img)))


if __name__ == "__main__":
    main()
