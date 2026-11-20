# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# X035 (PicoHub power manager, bus A 0x0B): stage-1 1.0.0 -> 1.1.0 over I2C, app 0.4.0, 0xB4 checks,
# then back to the released stage-1 1.0.0 + app 0.3.1. Files: s1_x035_110.bin, s1_x035_100.bin,
# picohub_040.bin, picohub_031.bin
import time
from noknok import Conductor
from module_flasher import ModuleFlasher
HUB = 0x0B
c = Conductor()
f = ModuleFlasher(c.i2c)
res = []

def crc8(d):
    x = 0
    for b in d:
        x ^= b
        for _ in range(8):
            x = ((x << 1) ^ 7) & 0xFF if x & 0x80 else (x << 1) & 0xFF
    return x

def id_type(addr, n=100):
    seen = set()
    for _ in range(n):
        buf = bytearray(10)
        while not c.i2c.try_lock(): pass
        try:
            c.i2c.writeto(addr, b"\xB4"); time.sleep(0.002); c.i2c.readfrom_into(addr, buf)
        except OSError:
            return None
        finally:
            c.i2c.unlock()
        if crc8(buf[:9]) != buf[9]:
            return None
        seen.add(bytes(buf[:9]))
    return seen.pop()[8] if len(seen) == 1 else None

def step(n, ok, i=""):
    res.append(ok); print("%s  %s  %s" % ("PASS" if ok else "FAIL", n, i))

def ld(n):
    with open(n, "rb") as fh: return fh.read()

def hub_back():
    for _ in range(30):
        time.sleep(0.5)
        try:
            c.enumerate_all()
        except Exception:
            pass
        if c.picohub is not None:
            return c.picohub
    return None

c.enumerate_all()
step("PicoHub enumerated at start", c.picohub is not None, "fw %s" % getattr(c.picohub, "firmware_version", "?"))
b, a = f.flash_stage1(ld("s1_x035_110.bin"), runtime_addr=HUB)
step("X035 stage-1 1.0.0 -> 1.1.0 over I2C", tuple(a) == (1, 1, 1, 0, 3), "%r -> %r" % (b, a))
t = id_type(0x7E)
step("0xB4 at 0x7E = fixed type 0x07 (no app)", t == 0x07, "type %r" % t)
f.flash(ld("picohub_040.bin"))
h = hub_back()
step("PicoHub app 0.4.0 runs", h is not None and h.firmware_version == "0.4.0", "fw %s" % getattr(h, "firmware_version", None))
t = id_type(h.address) if h else None
step("app 0xB4 x100 = 0x07", t == 0x07, "type %r" % t)
f.enter_bootloader(h.address); f.wait_for_bootloader()
t = id_type(0x7E)
step("0xB4 at 0x7E after 0xB0 = 0x07", t == 0x07, "type %r" % t)
# restore the released parts
b, a = f.flash_stage1(ld("s1_x035_100.bin"))
step("restore stage-1 1.0.0", tuple(a) == (1, 1, 0, 0, 3), "%r" % (a,))
f.flash(ld("picohub_031.bin"))
h = hub_back()
step("restore app 0.3.1", h is not None and h.firmware_version == "0.3.1", "fw %s" % getattr(h, "firmware_version", None))
print("SUMMARY: %d/%d PASS" % (sum(res), len(res)))
c.i2c.deinit()
