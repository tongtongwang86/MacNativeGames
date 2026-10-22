#!/usr/bin/env python3
"""A Ghidra-ready image of one game module (the game's own code, not the port's rewritten copy), for reading code while
working on the port. Nothing it writes is committed: it is made from the player's game files (build/ghidra/).

    ghidra_prep.py [--module main] [--out build/ghidra]

Writes <module>.flat: the module as loaded (text, rodata, data, bss) with its relocations applied for base 0 (RELATIVE
and defined symbols; each imported function gets an address in an "imports" area after the image), and
<module>.tsv: `seg <name> <start> <end> <perm>`, `fn <address>` (function starts from eh_frame_hdr) and
`sym <address> <name>` lines, read by scripts/ghidra/ImportModule.java."""
import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / '../totk/third_party/d2r'))
import nsoimg  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--module', default='main')
    ap.add_argument('--out', type=Path, default=HERE / 'build/ghidra')
    a = ap.parse_args()
    nso = (HERE / 'extracted/exefs' / a.module).read_bytes()
    segs = [struct.unpack_from('<III', nso, 0x10 + i * 0x10) for i in range(3)]
    img = bytearray(nsoimg.load(str(HERE / 'extracted/exefs' / a.module)))
    text_end, ro_start, ro_end, data_start = segs[0][2], segs[1][1], segs[1][1] + segs[1][2], segs[2][1]
    imports_base = (len(img) + 0xffff) & ~0xffff
    imports, syms = {}, []
    for where, typ, name, val, addend in nsoimg.relocs(bytes(img)):
        if typ == 1027:
            v = addend
        elif val is not None:
            v = val + (addend if typ != 1026 else 0)
            if name:
                syms.append((val, name))
        elif name:
            if name not in imports:
                imports[name] = imports_base + 16 * len(imports)
            v = imports[name] + (addend if typ != 1026 else 0)
        else:
            continue
        struct.pack_into('<Q', img, where, v & 0xffffffffffffffff)
    img += bytes(imports_base - len(img)) + bytes(16 * len(imports))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / f'{a.module}.flat').write_bytes(img)
    with open(a.out / f'{a.module}.tsv', 'w') as f:
        f.write(f'seg text 0x0 {text_end:#x} rx\nseg rodata {ro_start:#x} {ro_end:#x} r\n')
        f.write(f'seg data {data_start:#x} {imports_base:#x} rw\nseg imports {imports_base:#x} {len(img):#x} rx\n')
        for addr in nsoimg.func_starts(bytes(img), text_end):
            f.write(f'fn {addr:#x}\n')
        for addr, name in sorted(set(syms)):
            f.write(f'sym {addr:#x} {name}\n')
        for name, addr in imports.items():
            f.write(f'sym {addr:#x} {name}\n')
    print(f'{a.module}: {len(img):#x} bytes, {len(imports)} imports, {len(set(syms))} defined symbols -> {a.out}')


if __name__ == '__main__':
    main()
