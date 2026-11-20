# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# probe_getid.py - Pico (pico.py run): 0xB4 GET_ID check (ADR-005, DEV-21 / DEV-22).
# Reads the 10-byte identity [UID x8][TYPE][CRC8] N times from:
#   - a module parked in its stage-1 bootloader at 0x7E (stage-1 V003 >= 1.3.0, X035 >= 1.1.0)
#   - every module the Conductor enumerates (apps from DEV-21 on)
# and checks CRC-8 (poly 0x07), that the UID equals the enumeration UID / 0xB3 UID, and that
# the type is stable. Prints one PASS/FAIL line per address. N = first line below.
N = 100

import time
from noknok import Conductor
from module_flasher import ModuleFlasher


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def get_id(i2c, addr):
    buf = bytearray(10)
    while not i2c.try_lock():
        pass
    try:
        i2c.writeto(addr, bytes([0xB4]))
        time.sleep(0.002)
        i2c.readfrom_into(addr, buf)
    finally:
        i2c.unlock()
    return bytes(buf)


def check(i2c, addr, want_uid=None, label=""):
    ok = bad_crc = mism = err = 0
    types = set()
    first = None
    for _ in range(N):
        try:
            r = get_id(i2c, addr)
        except OSError:
            err += 1
            continue
        if crc8(r[:9]) != r[9]:
            bad_crc += 1
            continue
        first = first or r
        types.add(r[8])
        if want_uid and r[:8].hex() != want_uid:
            mism += 1
            continue
        ok += 1
    verdict = "PASS" if ok == N and len(types) == 1 else "FAIL"
    print("%s 0x%02X %s: %d/%d ok, crc bad %d, uid mismatch %d, i2c errors %d, types %s, uid %s"
          % (verdict, addr, label, ok, N, bad_crc, mism, err,
             ["0x%02X" % t for t in sorted(types)], first[:8].hex() if first else "-"))
    return verdict == "PASS"


c = Conductor()
f = ModuleFlasher(c.i2c)
st = f._read_status()
if st is not None:
    print("0x7E parked: state=%d err=%d, GET_VERSION %s" % (st[0], st[1], f.get_version()))
    check(c.i2c, 0x7E, want_uid=f.get_uid(), label="(bootloader, vs 0xB3)")
else:
    print("0x7E: nothing parked")
    n = c.enumerate_all()
    print("enumerated:", n)
    for name in ("buzzer", "knob", "ledbutton", "display", "picohub"):
        mods = getattr(c, name, None) or []
        if not isinstance(mods, list):
            mods = [mods]                       # c.picohub is a single object
        for m in mods:
            bus = c._bus(getattr(m, "i2c_bus", "A")) if hasattr(c, "_bus") else c.i2c
            check(bus, m.address, want_uid=getattr(m, "_uid_hex", None),
                  label="%s bus %s fw=%s" % (name, getattr(m, "i2c_bus", "A"),
                                             getattr(m, "firmware_version", "?")))
c.i2c.deinit()
