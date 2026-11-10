# Pico side of the bad-app test: is the module parked at 0x7E, and does it say why?
import board, busio
from module_flasher import ModuleFlasher, ST_ERROR
# Pins: the rig's settings.toml (bus A, then bus B if configured; the keys noknok.py uses), else the old bench
# wiring SDA=GP8 / SCL=GP9. The first bus where 0x7E answers is used (DEV-231: on the PicoHub rig the LED
# Button sits on GP20/GP21; the fixed GP8/GP9 gave "No pull up found" on 9 Nov 2026).
import os
cands = []
for k_sda, k_scl in (("NOKNOK_I2C_SDA", "NOKNOK_I2C_SCL"), ("NOKNOK_I2C_B_SDA", "NOKNOK_I2C_B_SCL")):
    a, b = os.getenv(k_sda), os.getenv(k_scl)
    if a and b:
        cands.append((getattr(board, a), getattr(board, b)))
if not cands:
    cands = [(board.GP8, board.GP9)]
i2c = f = st = None
for sda, scl in cands:
    try:
        i2c = busio.I2C(scl, sda, frequency=100_000)
    except Exception as e:                      # no pull-ups / pins busy: try the next bus
        print("bus", sda, scl, "skipped:", e)
        continue
    f = ModuleFlasher(i2c)
    st = f._read_status()
    if st is not None:
        break
    i2c.deinit()
if st is None:
    print("0x7E silent -> module is NOT in the bootloader (app still being booted?)")
    raise SystemExit(1)
state, err = st
print("0x7E status: state=%d err=%d  ->" % (state, err),
      "APP UNHEALTHY, parked" if (state == ST_ERROR and err == 7) else "unexpected")
v = f.get_version(); print("GET_VERSION:", v)
f._write(0x7E, [0xB3]); buf = bytearray(8)
while not i2c.try_lock(): pass
try:
    i2c.readfrom_into(0x7E, buf); print("GET_UID    :", buf.hex())
finally:
    i2c.unlock()
raise SystemExit(0 if (state == ST_ERROR and err == 7) else 1)
