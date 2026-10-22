#!/usr/bin/env python3
"""Lists every place main reads the session's player count (the byte at +0x476 of the object behind main+0x4ab1760;
"2 or more" = company is here), with its function and comparison: the map of ACNH's multiplayer restrictions
(docs/LAN-COOP-DESIGN.md section 8.7). Static, read-only on rewritten/main.patched.bin.
    mp_count_checks.py [rewritten/main.patched.bin]"""
import sys
import numpy as np

TEXT_END, GLOBAL_PAGE, GLOBAL_OFF, FIELD = 0x3f05ab0, 0x4ab1000, 0x760, 0x476
COND = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'nv']


def main():
    data = open(sys.argv[1] if len(sys.argv) > 1 else 'rewritten/main.patched.bin', 'rb').read()
    w = np.frombuffer(data[:TEXT_END - TEXT_END % 4], dtype='<u4')
    loads = np.nonzero(((w & 0xffc00000) == 0x39400000) & (((w >> 10) & 0xfff) == FIELD))[0]

    def function_start(i):
        while i > 0:
            x = int(w[i])
            if (x & 0xffc07fff) == 0xa9807bfd or (x & 0xff0003ff) == 0xd10003ff:
                return i * 4
            i -= 1
        return 0

    def from_session(i):   # adrp to the global's page, then ldr #0x760, within the 10 instructions before
        for j in range(max(0, i - 10), i):
            x = int(w[j])
            if (x & 0x9f000000) != 0x90000000:
                continue
            imm = ((x >> 5) & 0x7ffff) << 2 | ((x >> 29) & 3)
            imm -= (1 << 21) if imm & (1 << 20) else 0
            if ((j * 4) & ~0xfff) + (imm << 12) == GLOBAL_PAGE and any(
                    (int(w[k]) & 0xffc00000) == 0xf9400000 and ((int(w[k]) >> 10) & 0xfff) * 8 == GLOBAL_OFF for k in range(j + 1, i)):
                return True
        return False

    rows = []
    for i in loads:
        if not from_session(i):
            continue
        cmp, cond = None, ''
        for k in range(1, 4):
            x = int(w[i + k])
            if (x & 0x7f800000) == 0x71000000 and ((x >> 5) & 31) == (int(w[i]) & 31):
                cmp = (x >> 10) & 0xfff
            if (x & 0xff000010) == 0x54000000 and not cond:
                cond = COND[x & 15]
        rows.append((i * 4, function_start(i), cmp, cond))
    print(f'{len(rows)} session player-count checks in {len({r[1] for r in rows})} functions')
    for at, fn, cmp, cond in rows:
        print(f'main+{at:#x}\tfunction main+{fn:#x}\tcmp {cmp}\t{cond}')


if __name__ == '__main__':
    main()
