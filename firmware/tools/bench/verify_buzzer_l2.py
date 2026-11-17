# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# verify_buzzer_l2.py - Pico (pico.py run): S-tier smoke after a buzzer restore - enumerate, list modules, read the buzzer stage-1 version (0xB1). Pair of restore_buzzer_l2.sh.
# verify_buzzer_l2.py -- Pico side: enumerate, show every module, then read the
# buzzer's stage-1 version via Conductor.bootloader_version() (drops to BL,
# 0xB1, BOOT, re-enumerates). Single enumeration + one BL round trip.
from noknok import Conductor
from module_flasher import ModuleFlasher
c = Conductor()
st = ModuleFlasher(c.i2c)._read_status()
print("0x7E (parked bootloader):", "NONE" if st is None else st)
c.enumerate()
for name in ("buzzer", "knob", "ledbutton", "display"):
    for m in getattr(c, name, []):
        print("  %-9s addr=0x%02X uid=%s fw=%s" % (name, m.address,
              getattr(m, "_uid_hex", "?"), getattr(m, "firmware_version", "?")))
for m in c.buzzer:
    entry = {"bus": "i2c", "address": m.address, "uid": m._uid_hex, "type": "buzzer"}
    v = c.bootloader_version(entry)
    print("buzzer %s bootloader_version=%s layout=%s" % (m._uid_hex, v, c.layout_of(v)))
c.i2c.deinit()
