#!/usr/bin/env python3
"""Import an ACNH device save (Ryujinx / LibHac layout) into this port's profile, read-only on the source.

    import_save.py <save dir> --uid <32 hex digits> [--profile user-data]

<save dir> holds 0/ (the committed copy, used here), 1/ and ExtraData0/1, as Ryujinx and JKSV export it. The files of
0/ are copied into <profile>/save/device/; an existing device save is first moved to
<profile>/save/device.backup-<time>. --uid is the resident's account UID as stored in Villager0/personal.dat (16 bytes,
file order); it becomes the port's account (<profile>/account.uid), so the game recognises the resident. The device
save sizes come from ExtraData0 (data and journal size) into <profile>/save/device.size.
"""
import argparse
import shutil
import struct
import sys
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('save', type=Path)
    ap.add_argument('--uid', required=True)
    ap.add_argument('--profile', type=Path, default=Path(__file__).resolve().parents[1] / 'user-data')
    a = ap.parse_args()
    src = a.save / '0'
    if not (src / 'main.dat').is_file() or not (src / 'mainHeader.dat').is_file():
        sys.exit(f'{src}: not an ACNH save (main.dat / mainHeader.dat missing)')
    uid = bytes.fromhex(a.uid)
    if len(uid) != 16 or uid == bytes(16):
        sys.exit('--uid must be 32 hex digits and not zero')
    extra = (a.save / 'ExtraData0').read_bytes()
    owner, = struct.unpack_from('<Q', extra, 0x40)
    size, journal = struct.unpack_from('<qq', extra, 0x58)
    if owner != 0x01006F8002326000:
        sys.exit(f'ExtraData0 owner {owner:#x} is not ACNH (0x01006F8002326000)')

    save = a.profile / 'save'
    save.mkdir(parents=True, exist_ok=True)
    device = save / 'device'
    if device.exists() and any(device.iterdir()):
        backup = save / f'device.backup-{time.strftime("%Y%m%d-%H%M%S")}'
        device.rename(backup)
        print(f'existing device save moved to {backup}')
    elif device.exists():
        device.rmdir()
    shutil.copytree(src, device)
    (save / 'device.size').write_bytes(struct.pack('<qq', size, journal))
    (a.profile / 'account.uid').write_bytes(uid)
    files = sum(1 for p in device.rglob('*') if p.is_file())
    print(f'imported {files} files into {device}; device save {size:#x} + {journal:#x} journal; account set to the resident UID')


if __name__ == '__main__':
    main()
