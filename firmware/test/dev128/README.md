# DEV-128 bench test files (branch only, never merged)

- `noknok_stage1_x035_101.bin` - CH32X035 stage-1 built from main with
  `make CHIP=x035 EXTRA_CFLAGS=-DS1_VERSION_PATCH=1` (version bump only; 3392 B, CRC32 11a87ba8).
  The same source without the flag reproduces the published 1.0.0 byte for byte (CRC32 febe532f).
- `index_x035_101.json` - its index (layout 3).
- `registry_dev128.json` - the registry 2.0.0 shape (Ecosystem branch dev-128-stage1-per-chip) with the
  CH32X035 entry pointing at the 1.0.1 test index; the CH32V003 entry is the real one.

Bench use: point the brain's MODULE_REGISTRY_URL at
`https://raw.githubusercontent.com/buildwithnoknok/module-I2C-bootloader/dev-128-test/firmware/test/dev128/registry_dev128.json`
on the bench Pico only. Expected: the PicoHub X035 goes 1.0.0 -> 1.0.1 with its app restored; a CH32V003
module on the same brain is not touched (it is already on the published V003 stage-1).
After the test, put 1.0.0 back (Conductor.stage1_update with the published image).
