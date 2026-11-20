# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# bench_dev22_s1update.py - Pico (pico.py run): DEV-22 stage-1 self-update over I2C on a V003 module
# that starts on stage-1 1.2.0 with a DEV-21 app (SWD full image built WITHOUT a type word).
# Needs on CIRCUITPY: s1_130.bin, s1_131.bin (1.3.0 and a 1.3.1 test build), app_hdr.bin.
#   1.2.0 -> 1.3.0 : the old stage-1's ERASE wipes the metadata, so 0xB4 = type 0 until the app re-push
#   app re-push    : 0xB4 at 0x7E = the app's type
#   1.3.0 -> 1.3.1 : the new ERASE keeps the type, so 0xB4 = type right after the install
BUS, ADDR, TYPE = "B", 0x0D, 0x01

import time
from noknok import Conductor
from module_flasher import ModuleFlasher

c = Conductor()
bus = c.i2c_b if BUS == "B" else c.i2c
f = ModuleFlasher(bus)
results = []


def crc8(d):
    x = 0
    for b in d:
        x ^= b
        for _ in range(8):
            x = ((x << 1) ^ 7) & 0xFF if x & 0x80 else (x << 1) & 0xFF
    return x


def id_type(addr=0x7E, n=20):
    seen = set()
    for _ in range(n):
        buf = bytearray(10)
        while not bus.try_lock():
            pass
        try:
            bus.writeto(addr, b"\xB4")
            time.sleep(0.002)
            bus.readfrom_into(addr, buf)
        except OSError:
            return None
        finally:
            bus.unlock()
        if crc8(buf[:9]) != buf[9]:
            return None
        seen.add(bytes(buf[:9]))
    return seen.pop()[8] if len(seen) == 1 else None


def step(name, ok, info=""):
    results.append(ok)
    print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, info))


def find_addr(uid):
    c.enumerate_all()
    for name in ("buzzer", "knob", "ledbutton", "display"):
        for m in getattr(c, name, []) or []:
            if getattr(m, "_uid_hex", None) == uid:
                return m.address
    return None


def load(n):
    with open(n, "rb") as fh:
        return fh.read()


s130, s131, app = load("s1_130.bin"), load("s1_131.bin"), load("app_hdr.bin")

c.enumerate_all()                       # a fresh SWD flash leaves the module unassigned
mods = [m for m in c.buzzer if getattr(m, "i2c_bus", "A") == BUS] if TYPE == 1 else []
ADDR = mods[0].address if mods else ADDR
f.enter_bootloader(ADDR)
f.wait_for_bootloader()
uid = f.get_uid()
b, a = f.flash_stage1(s130)
step("stage-1 1.2.0 -> 1.3.0 over I2C", tuple(b)[:4] == (1, 1, 2, 0) and tuple(a) == (1, 1, 3, 0, 2), "%r -> %r" % (b, a))
t = id_type()
step("after the 1.2.0 ERASE: 0xB4 answers, type 0 (metadata wiped by the old stage-1)", t == 0, "type %r" % t)

f.flash(app)
time.sleep(1.5)
addr = find_addr(uid)
step("app re-pushed, enumerates", addr is not None, "addr %r" % addr)
f.enter_bootloader(addr)
f.wait_for_bootloader()
t = id_type()
step("0xB4 at 0x7E after the app install", t == TYPE, "type %r" % t)

b, a = f.flash_stage1(s131)
step("stage-1 1.3.0 -> 1.3.1 over I2C", tuple(a) == (1, 1, 3, 1, 2), "%r -> %r" % (b, a))
t = id_type()
step("type survives the stage-1 self-update (1.3.0 ERASE kept it)", t == TYPE, "type %r" % t)

b, a = f.flash_stage1(s130)
step("back to 1.3.0", tuple(a) == (1, 1, 3, 0, 2), "%r" % (a,))
f.flash(app)
time.sleep(1.5)
addr = find_addr(uid)
t = id_type(addr, 100) if addr else None
step("app restored, app 0xB4 x100", t == TYPE, "addr %r type %r" % (addr, t))
print("SUMMARY: %d/%d PASS" % (sum(results), len(results)))
c.i2c.deinit()
