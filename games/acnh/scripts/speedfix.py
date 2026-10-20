#!/usr/bin/env python3
"""60 FPS at normal game speed: the community "ACNH 60 FPS + SpeedFix" mod by tmouh (github.com/tmouh/acnh-60fps,
CC BY-NC 4.0). Its files are not in this repository: this script downloads them from one pinned commit into
tooling/acnh-60fps (local, .gitignore), checks each against the SHA-256 below, and builds the RomFS overlay.

    python3 scripts/speedfix.py          # fetch (once) and prepare

What the mod changes (its docs/how-it-works.md): ACNH steps its world once per frame with per-frame amounts tuned for
30 updates a second, and there is no global speed setting; 6,417 words of main (3.0.0: speedfix_main_300.pchtxt) and
1,437 + 107 parameters (romfs Pack/StaticParam.pack, three Bcsv tables) are rescaled so 60 updates a second play at
the 30 FPS game's pace. Its SDK "enabler" (present every vblank) has no counterpart here: NVN runs in the host, and
acnh_patches.c presents every 60 Hz tick instead (DM_PRESENT_INTERVAL=display).

tooling/acnh-60fps/
    speedfix_main_300.pchtxt     read by the runtime (acnh_patches.c), which checks its build ID against main's;
                                 its offsets are main+offset (offset_shift 0x100 is the step to NSO file offsets)
    romfs/Pack/StaticParam.pack  the game's own file with the mod's IPS patch (input and result checked)
    romfs/Bcsv/*.bcsv            the mod's three villager tables
The runtime reads replaced RomFS files from romfs/ first (ACNH_ROMFS_OVERLAY, runtime/romfs.c)."""
import hashlib, sys, urllib.parse, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIR = ROOT / 'tooling/acnh-60fps'
COMMIT = 'da32201a812d45b7129a51d13229ba964c00b8e7'   # v1.4.2, 2026-10-18
MOD = 'ryujinx/mods/contents/01006F8002326000/ACNH 60 FPS + SpeedFix'
FILES = {   # name: (path in the repository, SHA-256)
    'speedfix_main_300.pchtxt': (f'{MOD}/exefs/speedfix_main_300.pchtxt', '8d66820e08950e6dd138a16ed6ef9cc35bb22b04c9d2c1774f627ca25d6e1119'),
    'StaticParam.pack.ips': ('pack/StaticParam.pack.ips', '05a169b3ffeb00818a55c329dbee7999c9bc4f990447111882bd5d5e5dcd953d'),
    'ItemNpcFtrActionParam.bcsv': (f'{MOD}/romfs/Bcsv/ItemNpcFtrActionParam.bcsv', 'fa285b04f48774dadf6bcf1d8c608f8751c1825d1635870ec6e7f274e8e6688d'),
    'ItemNpcWherearenFtrActionParam.bcsv': (f'{MOD}/romfs/Bcsv/ItemNpcWherearenFtrActionParam.bcsv', 'c61bf2046ffcb46d3dc6c005c44abbf7584ed9cd43e40ab7c73e720c127188a3'),
    'NpcInterest.bcsv': (f'{MOD}/romfs/Bcsv/NpcInterest.bcsv', 'eed347d2511367bfdb7803e12d1314fb36a18c873e95306ea05ae473ecf6c7e9'),
    'LICENSE': ('LICENSE', '41003d4a74749c0220e33dd415042164b5a1093ed401f36277234f772d22d3d0'),
}
PACK_CLEAN = '4a3d6530d6430b6b967467f16784f91700a3cd8baad5f5b28fe3c6cfe4983298'     # 3.0.0 to 3.0.3 StaticParam.pack
PACK_60 = '7ea1622e9367b063ffe98d2acb03f1acd375b752d2a1024e8825607972e7c572'        # the mod's 60 FPS result


def sha(b): return hashlib.sha256(b).hexdigest()


def fetch():
    DIR.mkdir(parents=True, exist_ok=True)
    for name, (path, want) in FILES.items():
        f = DIR / name
        if f.is_file() and sha(f.read_bytes()) == want: continue
        url = f'https://raw.githubusercontent.com/tmouh/acnh-60fps/{COMMIT}/{urllib.parse.quote(path)}'
        data = urllib.request.urlopen(url, timeout=60).read()
        if sha(data) != want: sys.exit(f'{name}: downloaded file does not match the pinned SHA-256')
        f.write_bytes(data); print(f'fetched {name}')


def apply_ips(patch, data):
    if patch[:5] != b'PATCH': sys.exit('StaticParam.pack.ips: not an IPS file')
    buf = bytearray(data); i = 5
    while patch[i:i + 3] != b'EOF':
        off, size = int.from_bytes(patch[i:i + 3], 'big'), int.from_bytes(patch[i + 3:i + 5], 'big'); i += 5
        if size:
            buf[off:off + size] = patch[i:i + size]; i += size
        else:
            count = int.from_bytes(patch[i:i + 2], 'big'); buf[off:off + count] = patch[i + 2:i + 3] * count; i += 3
        if i > len(patch): sys.exit('StaticParam.pack.ips: truncated')
    return bytes(buf)


def prepare():
    pack = (ROOT / 'extracted/romfs/Pack/StaticParam.pack').read_bytes()
    if sha(pack) != PACK_CLEAN: sys.exit('extracted/romfs/Pack/StaticParam.pack is not 3.0.0 to 3.0.3\'s (SHA-256 differs)')
    out = apply_ips((DIR / 'StaticParam.pack.ips').read_bytes(), pack)
    if sha(out) != PACK_60: sys.exit('the patched StaticParam.pack does not match the mod\'s 60 FPS result')
    (DIR / 'romfs/Pack').mkdir(parents=True, exist_ok=True); (DIR / 'romfs/Bcsv').mkdir(parents=True, exist_ok=True)
    (DIR / 'romfs/Pack/StaticParam.pack').write_bytes(out)
    for name in ('ItemNpcFtrActionParam.bcsv', 'ItemNpcWherearenFtrActionParam.bcsv', 'NpcInterest.bcsv'):
        (DIR / 'romfs/Bcsv' / name).write_bytes((DIR / name).read_bytes())
    print(f'SpeedFix ready in {DIR}: main patch list, StaticParam.pack and 3 Bcsv tables (RomFS overlay)')


if __name__ == '__main__':
    fetch(); prepare()
