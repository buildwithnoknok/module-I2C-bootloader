# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# bench_stage1_buzzer.py — install a new stage-1 on the bench BUZZER through
# the Conductor (the real product path), then restore its app and read the
# bootloader version back as FIVE bytes. One-off for the stage-1 1.2.0 rollout
# (12 Sep 2026). Bench-only; not part of the regression.
#
# Files on CIRCUITPY: noknok_stage1_new.bin  (stage-1 1.2.0)
#                     buzzer_firmware.bin    (buzzer app 3.5.0, layout 2)
from noknok import Conductor
from module_flasher import ModuleFlasher, BL_ADDR

STAGE1_NEW = "noknok_stage1_new.bin"
APP_BIN    = "buzzer_firmware.bin"


def find_buzzer(c):
    c.enumerate()
    if not c.buzzer:
        raise SystemExit("FAIL: no buzzer enumerated")
    m = c.buzzer[0]
    return {"type": "buzzer", "bus": "i2c", "address": m.address,
            "uid": getattr(m, "_uid_hex", None)}


def read_five(c):
    """0xB1 at 0x7E read as five bytes: proto, major, minor, patch, layout."""
    f = ModuleFlasher(c.i2c)
    f._write(BL_ADDR, [0xB1])
    buf = bytearray(5)
    while not c.i2c.try_lock():
        pass
    try:
        c.i2c.readfrom_into(BL_ADDR, buf)
    finally:
        c.i2c.unlock()
    return tuple(buf)


c = Conductor()
entry = find_buzzer(c)
print("buzzer at 0x%02X  uid %s" % (entry["address"], entry["uid"]))
print("bootloader before:", c.bootloader_version(entry))
entry = find_buzzer(c)

with open(STAGE1_NEW, "rb") as f:
    s1 = f.read()
with open(APP_BIN, "rb") as f:
    app = f.read()
print("stage1_update:", c.stage1_update(entry, s1, app))

entry = find_buzzer(c)
print("buzzer back at 0x%02X" % entry["address"])

# Read the 5-byte reply: drop into the bootloader, ask, boot back.
f = ModuleFlasher(c.i2c)
f.enter_bootloader(entry["address"])
f.wait_for_bootloader()
v = read_five(c)
print("0xB1 at 0x7E (5 bytes):", v, "-> layout", v[4])
f.boot()
c.enumerate()
print("PASS" if (v[1], v[2], v[4]) == (1, 2, 2) and c.buzzer else "FAIL")
