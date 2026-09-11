# Pico side of the bad-app test: is the module parked at 0x7E, and does it say why?
import board, busio
from module_flasher import ModuleFlasher, ST_ERROR
i2c = busio.I2C(board.GP9, board.GP8, frequency=100_000)
f = ModuleFlasher(i2c)
st = f._read_status()
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
