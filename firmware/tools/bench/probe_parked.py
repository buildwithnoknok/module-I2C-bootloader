# SPDX-FileCopyrightText: 2026 noknok (Christopher Houben)
# SPDX-License-Identifier: MIT
#
# probe_parked.py - Pico (pico.py run): talk to a module parked in its bootloader at 0x7E (GET_VERSION, state) through ModuleFlasher.
from noknok import Conductor
from module_flasher import ModuleFlasher, _ERRMSG
c = Conductor()
f = ModuleFlasher(c.i2c)
st = f._read_status()
if st is None:
    print("0x7E: nothing parked")
else:
    state, err = st
    print("0x7E: PARKED state=%d err=%d (%s)" % (state, err, _ERRMSG.get(err, "?")))
    v = f.get_version()
    print("  bootloader GET_VERSION:", v)
    print("  bootloader GET_UID:", f.get_uid())
n = c.enumerate_all()
print("enumerated:", n)
for name in ("buzzer", "knob", "ledbutton"):
    for m in getattr(c, name, []):
        print("  %-9s addr=0x%02X uid=%s fw=%s" % (name, m.address, getattr(m, "_uid_hex", "?"), getattr(m, "firmware_version", "?")))
c.i2c.deinit()
