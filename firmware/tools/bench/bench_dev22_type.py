# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# bench_dev22_type.py - Pico (pico.py run): DEV-22 stage-1 1.3.0 identity tests on a V003 module
# that runs a DEV-21 app (header, type 0x01 for the Buzzer). Needs on CIRCUITPY:
#   app_hdr.bin   (the DEV-21 app, e.g. Buzzer 3.6.0)      app_nohdr.bin (an app without header)
# Settings below: the module's bus/address, and the PicoHub address on bus A (its CYCLE_RAIL
# 0x21 cuts the module's bus rail = the power cut). Prints PASS/FAIL per step.
BUS, ADDR, TYPE = "B", 0x0D, 0x01
HUB, RAIL = 0x0B, 1          # PicoHub at 0x0B (bus A); rail 1 = bus B

import time
from noknok import Conductor
from module_flasher import ModuleFlasher, crc32, PAGE

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


def get_id(addr=0x7E):
    buf = bytearray(10)
    while not bus.try_lock():
        pass
    try:
        bus.writeto(addr, b"\xB4")
        time.sleep(0.002)
        bus.readfrom_into(addr, buf)
    finally:
        bus.unlock()
    return bytes(buf)


def id_type(n=20, addr=0x7E):
    """0xB4 n times; returns the type if every reply has a good CRC and the same UID+type, else None."""
    seen = set()
    for _ in range(n):
        try:
            r = get_id(addr)
        except OSError:
            return None
        if crc8(r[:9]) != r[9]:
            return None
        seen.add(r[:9])
    if len(seen) != 1:
        return None
    r = seen.pop()
    uid = f.get_uid() if addr == 0x7E else None
    if uid and r[:8].hex() != uid:
        return None
    return r[8]


def step(name, ok, info=""):
    results.append((name, ok))
    print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, info))


def cut(ms=600):
    """Power-cycle the module's bus via the PicoHub (CYCLE_RAIL, key 0x5A)."""
    while not c.i2c.try_lock():
        pass
    try:
        c.i2c.writeto(HUB, bytes([0x21, RAIL, ms & 0xFF, ms >> 8, 0x5A]))
    finally:
        c.i2c.unlock()
    time.sleep(ms / 1000 + 1.0)


def find_addr(uid):
    """Runtime address of the module with this UID after (re-)enumeration."""
    c.enumerate_all()
    for name in ("buzzer", "knob", "ledbutton", "display"):
        for m in getattr(c, name, []) or []:
            if getattr(m, "_uid_hex", None) == uid:
                return m.address
    return None


def load(name):
    with open(name, "rb") as fh:
        return fh.read()


app_hdr, app_nohdr = load("app_hdr.bin"), load("app_nohdr.bin")

# 1. parked: 0xB4 at 0x7E, version
f.enter_bootloader(ADDR)
f.wait_for_bootloader()
UID = f.get_uid()
v = f.get_version()
step("0xB1 at 0x7E = 1.3.0 layout 2", tuple(v or ()) == (1, 1, 3, 0, 2), str(v))
t = id_type(100)
step("0xB4 at 0x7E x100, type from metadata", t == TYPE, "type %r" % t)

# 2. ERASE keeps the type; power cut after ERASE
f.erase()
st = f._read_status()
t = id_type()
step("after ERASE: READY, type kept", st == (2, 0) and t == TYPE, "status %r type %r" % (st, t))
cut()
st = f._read_status()
t = id_type()
step("power cut after ERASE: parked, type kept", st is not None and st[0] != 2 and t == TYPE,
     "status %r type %r" % (st, t))

# 3. power cut mid-transfer (ERASE + 20 chunks)
f.erase()
for off in range(0, 20 * PAGE, PAGE):
    f.write_chunk(off, app_hdr[off:off + PAGE])
cut()
st = f._read_status()
t = id_type()
step("power cut mid-transfer: parked, type kept", st is not None and t == TYPE, "status %r type %r" % (st, t))

# 4. wrong file: the same app with target "X035" in its header -> error 9, nothing booted
bad = bytearray(app_hdr)
bad[0x104:0x108] = b"X035"
try:
    f.erase()
    for off in range(0, len(bad), PAGE):
        f.write_chunk(off, bad[off:off + PAGE])
    f.verify(len(bad), crc32(bytes(bad)))
    step("X035-header app refused (error 9)", False, "VERIFY accepted it")
except Exception as e:
    st = f._read_status()
    step("X035-header app refused (error 9)", st == (3, 9), "%s / status %r" % (e, st))
f.boot()
time.sleep(0.3)
st = f._read_status()
step("BOOT after refusal: stays parked (error 6)", st == (3, 6), "status %r" % (st,))

# 5. app without header installs, type 0
f.flash(app_nohdr)
time.sleep(1.5)
ADDR = find_addr(UID)
step("headerless app boots and enumerates", ADDR is not None, "addr %r" % ADDR)
f.enter_bootloader(ADDR)
f.wait_for_bootloader()
t = id_type()
step("headerless app: 0xB4 type 0 at 0x7E", t == 0, "type %r" % t)

# 6. back to the header app via the normal path; app answers 0xB4 itself
f.flash(app_hdr)
time.sleep(1.5)
ADDR = find_addr(UID)
t = id_type(100, ADDR) if ADDR else None
step("DEV-21 app back, app 0xB4 x100", t == TYPE, "type %r" % t)

print("SUMMARY: %d/%d PASS" % (sum(1 for _, ok in results if ok), len(results)))
c.i2c.deinit()
