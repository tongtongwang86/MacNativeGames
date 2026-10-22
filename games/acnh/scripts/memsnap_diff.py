#!/usr/bin/env python3
"""Find a value the game keeps in memory from snapshots taken with the `memsnap` diagnostic command (runtime/mem_diag.c)
while the player does something known in between, e.g. stands still, walks down, walks right:

    memsnap_diff.py SNAP0 SNAP1 SNAP2 ... [--still 0,1] [--min 1] [--max 100000] [--step 0.3]

Prints the float32 words (address and value in every snapshot) that are equal in the --still snapshots, are finite with
an absolute value in [--min, --max] in all of them, and move by at least --step somewhere after. Rows come out by
address, so a position shows up as consecutive x, y, z words. How docs/GAME-MAP.md found the player coordinates."""
import argparse
import math
import struct
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('snaps', nargs='+', type=Path)
    ap.add_argument('--still', default='0,1')
    ap.add_argument('--min', type=float, default=1)
    ap.add_argument('--max', type=float, default=100000)
    ap.add_argument('--step', type=float, default=0.3)
    a = ap.parse_args()
    still = [int(x) for x in a.still.split(',')]
    for region in sorted(p.name for p in a.snaps[0].glob('*.bin')):
        files = [s / region for s in a.snaps]
        if not all(f.exists() for f in files):
            continue
        data = [f.read_bytes() for f in files]
        n = min(len(d) for d in data) & ~3
        base = int(region[:-4], 16)
        chunk = 1 << 16
        for o in range(0, n, chunk):
            parts = [d[o:min(o + chunk, n)] for d in data]
            if all(p == parts[0] for p in parts[1:]):
                continue
            k = len(parts[0]) // 4
            vals = [struct.unpack(f'<{k}f', p[:k * 4]) for p in parts]
            for i in range(k):
                v = [vs[i] for vs in vals]
                if any(v[s] != v[still[0]] for s in still):
                    continue
                if not all(math.isfinite(x) and a.min <= abs(x) <= a.max for x in v):
                    continue
                if max(abs(v[j] - v[j - 1]) for j in range(1, len(v))) < a.step:
                    continue
                print(f'{base + o + i * 4:#x}', ' '.join(f'{x:10.3f}' for x in v))


if __name__ == '__main__':
    main()
