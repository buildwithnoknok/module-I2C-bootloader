#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
#
# Build the stage-0 bench test images (DEV-31).
#
# Produces two 16 KB SWD-flashable images that exercise stage-0's two paths.
# In both, the witness page at 0x3C00 tells you which stage-1 actually ran.
#
#   test_jump.bin    stage-0 + fake stage-1 "A1" installed at 0x0400, no update
#                    pending.  EXPECT witness == 0xA1  (stage-0 jumped to it)
#
#   test_update.bin  same, PLUS fake stage-1 "A2" staged at 0x1000 and a valid
#                    control block at 0x3F80.
#                    EXPECT witness == 0xA2, stage-1 region == A2, marker cleared
#                    (stage-0 applied the pending update, then booted the result)
#
# Run on the Pi from firmware/stage0/test/ with ../noknok_stage0.bin already built.

import os, struct, subprocess, sys, zlib

HERE       = os.path.dirname(os.path.abspath(__file__))
STAGE0_BIN = os.path.join(HERE, '..', 'noknok_stage0.bin')


def find_ch32fun(start):
    """Walk up looking for the dir holding ch32fun.h — the repo checkout and the
    Pi build tree nest this differently, so don't hardcode a relative depth."""
    d = start
    for _ in range(8):
        cand = os.path.join(d, 'ch32fun')
        if os.path.exists(os.path.join(cand, 'ch32fun.h')):
            return os.path.abspath(cand)
        d = os.path.dirname(d)
    sys.exit('could not locate ch32fun/ch32fun.h above ' + start)


CH32FUN = find_ch32fun(HERE)

FLASH_SIZE   = 0x4000
STAGE1_BASE  = 0x0400          # execution/file offset — MUST be 1 KB aligned (mtvec)
APP_BASE     = 0x1000
CTRL_OFF     = 0x3F80
META_OFF     = 0x3FC0
WITNESS      = 0x3C00
CTRL_MAGIC   = 0x6E6B5530
FLASH_ALIAS  = 0x08000000

CFLAGS = ('-g -Os -flto -ffunction-sections -fdata-sections -fmessage-length=0 '
          '-msmall-data-limit=8 -fno-tree-loop-distribute-patterns '
          '-march=rv32ec -mabi=ilp32e -DCH32V003 -static-libgcc -nostdlib -Wall '
          '-I. -I.. -I{cf} -I{cf}/../extralibs -I/usr/include/newlib').format(cf=CH32FUN)


def build_fake(stage1_id, out, ld='fake_stage1.ld'):
    """Compile the fake stage-1 with a given witness ID."""
    elf = out.replace('.bin', '.elf')
    cmd = (f'riscv64-unknown-elf-gcc -o {elf} ../start.S fake_stage1.c '
           f'{CFLAGS} -DSTAGE1_ID={stage1_id:#010x} '
           f'-T {ld} -Wl,--gc-sections')
    subprocess.run(cmd, shell=True, cwd=HERE, check=True)
    subprocess.run(f'riscv64-unknown-elf-objcopy -O binary {elf} {out}',
                   shell=True, cwd=HERE, check=True)
    with open(os.path.join(HERE, out), 'rb') as f:
        return f.read()


def blank():
    return bytearray(b'\xff' * FLASH_SIZE)


def place(img, off, data, what):
    end = off + len(data)
    assert end <= FLASH_SIZE, f'{what} overflows flash'
    img[off:end] = data
    print(f'  {what:22s} @ 0x{off:04X}  {len(data):5d} B  (ends 0x{end:04X})')


def main():
    if not os.path.exists(STAGE0_BIN):
        sys.exit('build stage-0 first: (cd .. && make build)')
    stage0 = open(STAGE0_BIN, 'rb').read()

    # stage-0 must not run into the stage-1 base, or it would erase itself.
    assert len(stage0) <= STAGE1_BASE, (
        f'stage-0 is {len(stage0)} B but stage-1 starts at 0x{STAGE1_BASE:04X}')

    print('building fake stage-1 images...')
    a1 = build_fake(0xA1, 'fake_a1.bin')
    a2 = build_fake(0xA2, 'fake_a2.bin')
    assert a1 != a2, 'the two fake stage-1 builds are identical - test is blind'

    # ---- test_jump: no update pending, stage-0 should just run A1 ----------
    print('\ntest_jump.bin')
    img = blank()
    place(img, 0,           stage0, 'stage-0')
    place(img, STAGE1_BASE, a1,     'fake stage-1 A1')
    open(os.path.join(HERE, 'test_jump.bin'), 'wb').write(img)

    # ---- test_update: A2 staged in the app region + a valid control block --
    print('\ntest_update.bin')
    img = blank()
    place(img, 0,           stage0, 'stage-0')
    place(img, STAGE1_BASE, a1,     'fake stage-1 A1')
    place(img, APP_BASE,    a2,     'staged stage-1 A2')

    ctrl = struct.pack('<5I',
                       CTRL_MAGIC,                 # update pending
                       FLASH_ALIAS + APP_BASE,     # staging_addr
                       len(a2),                    # stage1_len
                       zlib.crc32(a2) & 0xffffffff,# stage1_crc32 (stage-1's job)
                       FLASH_ALIAS + APP_BASE)     # app_base
    place(img, CTRL_OFF, ctrl, 'control block')
    open(os.path.join(HERE, 'test_update.bin'), 'wb').write(img)

    # ---- test_recover: what a power cut MID-COPY actually leaves behind -----
    # Stage-1 half written, staging untouched, marker still set. This is the
    # state DEV-31's core acceptance criterion is about: stage-0 must notice the
    # update is still pending and simply do the whole copy again.
    # The witness is pre-set to 0x00 so a stale 0xA2 from an earlier test cannot
    # be mistaken for a fresh successful recovery.
    print('\ntest_recover.bin  (simulated power cut mid-copy)')
    img = blank()
    place(img, 0,           stage0,          'stage-0')
    place(img, STAGE1_BASE, a2[:len(a2)//2], 'HALF-written stage-1')
    place(img, APP_BASE,    a2,              'staged stage-1 A2 (intact)')
    place(img, WITNESS,     b'\x00' * 64,    'witness cleared')
    place(img, CTRL_OFF,    ctrl,            'control block (marker STILL SET)')
    open(os.path.join(HERE, 'test_recover.bin'), 'wb').write(img)

    # ---- The other three power-loss windows (spec §5) --------------------------
    # test_recover covered "power cut during erase/copy". These cover the rest.
    # In every one, stage-0 must NOT get stuck and must boot SOME stage-1 cleanly.
    # The witness is pre-cleared to 0x00 in all of them so a stale value from an
    # earlier test cannot masquerade as a pass.

    # Window 1 — cut during the STAGING transfer: half an image at 0x1000, and no
    # marker (stage-1 only writes the marker after a full CRC-verified transfer).
    # Expect: stage-0 sees no marker, boots the installed A1 untouched.
    print('\ntest_stage_cut.bin  (power cut mid-staging: partial image, no marker)')
    img = blank()
    place(img, 0,           stage0,          'stage-0')
    place(img, STAGE1_BASE, a1,              'installed stage-1 A1')
    place(img, APP_BASE,    a2[:len(a2)//2], 'HALF-staged A2 (no marker)')
    place(img, WITNESS,     b'\x00' * 64,    'witness cleared')
    open(os.path.join(HERE, 'test_stage_cut.bin'), 'wb').write(img)

    # Window 2a — cut during the MARKER write, before the magic landed: the
    # descriptor fields are valid but word 0 is still 0xFFFFFFFF.
    # Expect: not a pending update -> boots A1.
    print('\ntest_marker_nomagic.bin  (marker write cut before the magic)')
    img = blank()
    place(img, 0,           stage0,       'stage-0')
    place(img, STAGE1_BASE, a1,           'installed stage-1 A1')
    place(img, APP_BASE,    a2,           'fully staged A2')
    place(img, WITNESS,     b'\x00' * 64, 'witness cleared')
    nomagic = struct.pack('<5I', 0xFFFFFFFF,
                          FLASH_ALIAS + APP_BASE, len(a2),
                          zlib.crc32(a2) & 0xffffffff, FLASH_ALIAS + APP_BASE)
    place(img, CTRL_OFF, nomagic, 'control block: magic FF, rest valid')
    open(os.path.join(HERE, 'test_marker_nomagic.bin'), 'wb').write(img)

    # Window 2b — the nasty one: the magic landed but the descriptor did not.
    # To a naive implementation this LOOKS like a pending update. Stage-0's
    # sanity check must reject it (app_base 0xFFFFFFFF > FLASH_END, len
    # 0xFFFFFFFF > region) and fall through to booting A1. The marker stays set
    # (stage-0 never clears one it did not act on) — that is documented and
    # harmless: stage-1 rewrites the block on the next real update.
    print('\ntest_marker_garbage.bin  (marker write cut after the magic)')
    img = blank()
    place(img, 0,           stage0,       'stage-0')
    place(img, STAGE1_BASE, a1,           'installed stage-1 A1')
    place(img, APP_BASE,    a2,           'fully staged A2')
    place(img, WITNESS,     b'\x00' * 64, 'witness cleared')
    garbage = struct.pack('<5I', CTRL_MAGIC, 0xFFFFFFFF, 0xFFFFFFFF,
                          0xFFFFFFFF, 0xFFFFFFFF)
    place(img, CTRL_OFF, garbage, 'control block: magic OK, descriptor FF')
    open(os.path.join(HERE, 'test_marker_garbage.bin'), 'wb').write(img)

    # Window 4 — cut during the MARKER CLEAR: the copy finished (stage-1 == A2)
    # but the marker is still fully set. Expect: stage-0 redoes the copy (A2 ->
    # A2, idempotent), clears the marker, boots A2.
    print('\ntest_clear_cut.bin  (power cut during marker clear: copy done, marker still set)')
    img = blank()
    place(img, 0,           stage0,       'stage-0')
    place(img, STAGE1_BASE, a2,           'stage-1 already == A2')
    place(img, APP_BASE,    a2,           'staged A2 (intact)')
    place(img, WITNESS,     b'\x00' * 64, 'witness cleared')
    place(img, CTRL_OFF,    ctrl,         'control block (marker STILL SET)')
    open(os.path.join(HERE, 'test_clear_cut.bin'), 'wb').write(img)

    # ---- test_chain: the REAL stage-1, not a stub ---------------------------
    # stage-0 -> real stage-1 -> application. Proves stage-0 hands off correctly
    # to the actual bootloader, that stage-1 runs from 0x0400, validates the app
    # against its metadata, and jumps to it. The "app" is the same witness
    # harness relinked to the app base, so one read confirms the whole chain.
    s1 = None
    for cand in ('../../noknok_stage1/noknok_stage1.bin',
                 '../../../stage1/noknok_stage1.bin'):
        p = os.path.join(HERE, cand)
        if os.path.exists(p):
            s1 = open(p, 'rb').read()
            break

    if s1 is None:
        print('\ntest_chain.bin SKIPPED - build stage-1 first')
    else:
        assert len(s1) <= APP_BASE - STAGE1_BASE, 'stage-1 overflows its region'
        app = build_fake(0xA3, 'fake_app.bin', ld='fake_app.ld')

        print('\ntest_chain.bin  (stage-0 -> REAL stage-1 -> app)')
        img = blank()
        place(img, 0,           stage0,       'stage-0')
        place(img, STAGE1_BASE, s1,           'REAL stage-1')
        place(img, APP_BASE,    app,          'fake app A3')
        place(img, WITNESS,     b'\x00' * 64, 'witness cleared')
        meta = struct.pack('<3I', 0xB007C0DE, len(app),
                           zlib.crc32(app) & 0xffffffff)
        place(img, META_OFF, meta, 'app metadata')
        open(os.path.join(HERE, 'test_chain.bin'), 'wb').write(img)

    print(f'\nwitness page 0x{WITNESS:04X}:')
    print('  test_jump    -> expect 0xA1  (stage-0 jumped to the installed stage-1)')
    print('  test_update  -> expect 0xA2  (stage-0 applied the pending update)')
    print('  test_recover -> expect 0xA2  (stage-0 redid an interrupted copy)')
    print('  test_stage_cut      -> expect 0xA1  (no marker: staged junk ignored)')
    print('  test_marker_nomagic -> expect 0xA1  (magic missing: not pending)')
    print('  test_marker_garbage -> expect 0xA1  (magic OK, descriptor insane: rejected)')
    print('  test_clear_cut      -> expect 0xA2  (marker still set: copy redone, harmless)')
    if s1 is not None:
        print('  test_chain   -> expect 0xA3  (stage-0 -> real stage-1 -> app)')
    print(f'\nstage-0 {len(stage0)} B / {STAGE1_BASE} B budget, '
          f'A1 {len(a1)} B, A2 {len(a2)} B'
          + (f', real stage-1 {len(s1)} B' if s1 else ''))


if __name__ == '__main__':
    main()
