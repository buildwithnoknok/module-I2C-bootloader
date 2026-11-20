# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# DEV-21: OTA the 0xB4 apps onto the modules (whatever stage-1 they run), check 0xB1 + 0xB4 x100.
# MODE = "new" pushes the DEV-21 bins, "old" restores the released bins. Files: <name>_new.bin / _old.bin
MODE = "new"
import time
from noknok import Conductor
from module_flasher import ModuleFlasher
WANT = {"knob": (0x02, "2.4.0", "2.3.0"), "ledbutton": (0x03, "2.5.0", "2.4.1"), "display": (0x05, "0.8.0", "0.7.1")}
c = Conductor()
res = []

def crc8(d):
    x = 0
    for b in d:
        x ^= b
        for _ in range(8):
            x = ((x << 1) ^ 7) & 0xFF if x & 0x80 else (x << 1) & 0xFF
    return x

def id_type(bus, addr, n=100):
    seen = set()
    for _ in range(n):
        buf = bytearray(10)
        while not bus.try_lock(): pass
        try:
            bus.writeto(addr, b"\xB4"); time.sleep(0.002); bus.readfrom_into(addr, buf)
        except OSError:
            return None
        finally:
            bus.unlock()
        if crc8(buf[:9]) != buf[9]:
            return None
        seen.add(bytes(buf[:9]))
    return seen.pop()[8] if len(seen) == 1 else None

def step(n, ok, i=""):
    res.append(ok); print("%s  %s  %s" % ("PASS" if ok else "FAIL", n, i))

c.enumerate_all()
for name, (typ, vnew, vold) in WANT.items():
    mods = getattr(c, name, []) or []
    if not mods:
        step(name + " present", False); continue
    m = mods[0]; uid = m._uid_hex; busn = getattr(m, "i2c_bus", "A")
    bus = c._bus(busn)
    f = ModuleFlasher(bus)
    with open("%s_%s.bin" % (name, MODE), "rb") as fh:
        data = fh.read()
    f.enter_bootloader(m.address); f.wait_for_bootloader()
    blv = f.get_version()
    f.flash(data)
    time.sleep(1.5)
    m2 = None
    for _ in range(10):
        c.enumerate_all()
        m2 = [x for x in getattr(c, name, []) or [] if x._uid_hex == uid]
        if m2: m2 = m2[0]; break
        time.sleep(0.5)
    want_v = vnew if MODE == "new" else vold
    step("%s OTA to %s (bootloader %r)" % (name, want_v, blv), bool(m2) and m2.firmware_version == want_v,
         "fw %s" % (m2.firmware_version if m2 else None))
    if MODE == "new" and m2:
        t = id_type(bus, m2.address)
        step("%s app 0xB4 x100 = 0x%02X" % (name, typ), t == typ, "type %r" % t)
print("SUMMARY: %d/%d PASS" % (sum(res), len(res)))
c.i2c.deinit()
