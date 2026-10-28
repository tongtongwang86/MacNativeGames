#!/usr/bin/env python3
"""Launch-screen pattern: turns the player's own picture of the swirl pattern (a 2 x 2 repeat of a 369-pixel tile, the
one the boot screen was designed with) into what runtime/boot_view_ios.m draws: a two-tone tile at 4 x its pixels and
one image per circle, each turned on its own at run time.

    python3 games/totk/ios/tools/launch_pattern.py <picture.jpg> <out folder>

Writes <out>/tile.png, <out>/circle-<name>.png and <out>/pattern.json. The picture is not in the repository (it is the
game's art); build_ipa.py --launch-pattern <out folder> copies the folder into the app as GameData/launch.

The pattern is two colours (lines on a near-black ground), so the tile is enlarged by interpolation and every pixel put
back on one of the two colours with a narrow anti-aliased edge: the shapes are the picture's, the edges are sharp at the
phone's 3 x. The tile's origin, its period and the five circles (centre, outer radius including the black outline)
were measured on that picture: the period is where the picture best matches itself shifted (369 px both ways), the
origin the window whose wrap-around seam is smallest, each centre where the rings are most alike in every direction,
each radius the outermost all-dark ring. Circle F lies behind the other four, so its image leaves their area clear.
"""
import json, sys
from pathlib import Path

import cv2
import numpy as np

PERIOD = 369                     # the tile, in the picture's pixels
ORIGIN = (164, 148)              # its top-left corner in the picture
SCALE = 4                        # tile.png is PERIOD * SCALE pixels square
STRIPES = 28                     # wavy vertical lines across one tile (13.2 px apart in the picture)
LINE_W = 7.1                     # their width (tile pixels)
LINE_STRIP = 14                  # each line's own image: this wide round its centre, room for its width and sway
EDGE_OUT = 1.5                   # how far past its radius a circle's traced outer edge may reach (tile pixels): the
                                 # outline wobbles about this much; further out it is a dark gap between stripes
DARK = (0x0e, 0x07, 0x05)        # the ground and the lines, sampled from the player's wallpaper (RGB)
LINE = (0x3a, 0x2f, 0x2e)
# name, centre x, centre y, outer radius (tile pixels), behind the others
CIRCLES = [
    ('F', 361.75, 8.75, 130.0, True),
    ('A', 102.25, 166.75, 95.0, False),
    ('B', 232.75, 363.75, 95.0, False),
    ('D', 284.25, 203.0, 62.0, False),
    ('C', 322.25, 104.25, 38.5, False),
]


def two_tone(gray):
    """Enlarged grey tile -> RGB in the two colours, with a 1.5-pixel anti-aliased edge at the midpoint."""
    lo, hi = np.percentile(gray, 20), np.percentile(gray, 85)
    t = (lo + hi) / 2
    a = np.clip((gray - (t - 6)) / 12, 0, 1)
    a = a * a * (3 - 2 * a)
    d, l = np.array(DARK, float), np.array(LINE, float)
    return (d[None, None] * (1 - a[..., None]) + l[None, None] * a[..., None]).round().astype(np.uint8)


def stripe_lines(size):
    """The wavy vertical lines under the circles, drawn on their own (the picture shows them only between the circles):
    STRIPES lines across the tile, each line's centre swaying with waves of whole cycles per tile and a little
    roughness, so each line wraps top to bottom and can scroll by itself. Widths and sway as measured in the picture's
    gaps: 13.2 px apart, 7.1 px wide, drifting 1 px sideways over 8 rows and 3 px over 64.
    Returns, per line, its centre (pixels across the tile) and its coverage in a strip LINE_STRIP wide around it."""
    rng = np.random.default_rng(7)
    S = size / PERIOD
    period = size / STRIPES
    strip = int(round(LINE_STRIP * S))
    y = np.arange(size, dtype=np.float64)
    out = []
    for k in range(STRIPES):
        p = rng.uniform(0, 2 * np.pi, 6)
        c0 = (k + 0.5) * period
        sway = S * (1.6 * np.sin(2 * np.pi * 3 * y / size + p[0]) + 0.8 * np.sin(2 * np.pi * 8 * y / size + p[1])
                    + 0.3 * np.sin(2 * np.pi * 21 * y / size + p[2]) + 0.08 * np.sin(2 * np.pi * 47 * y / size + p[3]))
        half = S * (LINE_W / 2) * (1 + 0.07 * np.sin(2 * np.pi * 5 * y / size + p[4]) + 0.025 * np.sin(2 * np.pi * 33 * y / size + p[5]))
        x = np.arange(strip, dtype=np.float64) + 0.5 - strip / 2          # across the strip, 0 = the line's resting centre
        d = np.abs(x[None, :] - sway[:, None]) - half[:, None]
        out.append((c0, np.clip(0.5 - d / 1.2, 0, 1)))                   # inside: 1, a 1.2-pixel anti-aliased edge
    return out


def stripes(size):
    """All the lines in one tile (for a still ground, and to check the lines against the picture)."""
    period_px = size
    a = np.zeros((size, size))
    for c0, cov in stripe_lines(size):
        w = cov.shape[1]
        xs = (np.arange(w) + int(round(c0 - w / 2))) % period_px
        a[:, xs] = np.maximum(a[:, xs], cov)
    d, l = np.array(DARK, float), np.array(LINE, float)
    return (d[None, None] * (1 - a[..., None]) + l[None, None] * a[..., None]).round().astype(np.uint8)


def traced_edge(gray, ccx, ccy, r, others, A=2048):
    """The circle's own outer edge, hand-drawn and jagged, as a radius per angle (pixels of the enlarged image): out
    from inside its black outline to the first light pixel (where the stripes, or a circle behind, begin). Angles where
    nothing light comes within EDGE_OUT past the radius (a dark gap between stripes) or where the edge lies under a
    circle in front (`others`: centres and radii) are filled from the traced angles on either side."""
    S = SCALE
    rho = np.arange(int((r - 2) * S), int((r + EDGE_OUT) * S))
    th = np.arange(A) * 2 * np.pi / A
    xs = (ccx + rho[None, :] * np.cos(th)[:, None]).astype(np.float32)
    ys = (ccy + rho[None, :] * np.sin(th)[:, None]).astype(np.float32)
    v = cv2.remap(gray, xs, ys, cv2.INTER_LINEAR)
    lo, hi = np.percentile(gray, 20), np.percentile(gray, 85)
    light = v > (lo + hi) / 2
    first = np.where(light.any(axis=1), light.argmax(axis=1), -1)
    edge = np.where(first >= 0, rho[0] + first, np.nan).astype(float)
    ex, ey = ccx + r * S * np.cos(th), ccy + r * S * np.sin(th)          # the nominal edge point at each angle
    for ox, oy, orr in others:
        edge[np.hypot(ex - ox, ey - oy) < orr * S + 2 * S] = np.nan
    ok = ~np.isnan(edge)
    if ok.sum() < 8:
        return np.full(A, r * S)
    idx = np.arange(A)
    edge = np.interp(idx, np.concatenate([idx[ok] - A, idx[ok], idx[ok] + A]), np.tile(edge[ok], 3))
    # a 9-sample median keeps the hand-drawn wobble and drops stray spikes
    pad = np.concatenate([edge[-4:], edge, edge[:4]])
    return np.median(np.stack([pad[i:i + A] for i in range(9)]), axis=0)


def fill_hidden(crop, seen, R):
    """crop: a disk image 2R square, seen: where its own rings show. In polar form (rows = angle, columns = radius) each
    column's hidden rows take the nearest seen row, wrapping round; back to the square, the filled pixels go in."""
    A = 2048
    centre = (R, R)
    flags = cv2.WARP_POLAR_LINEAR | cv2.INTER_LINEAR
    polar = cv2.warpPolar(crop, (R, A), centre, R, flags)
    vis = cv2.warpPolar(seen.astype(np.uint8) * 255, (R, A), centre, R, cv2.WARP_POLAR_LINEAR | cv2.INTER_NEAREST) > 127
    for c in range(R):
        col_vis = vis[:, c]
        if col_vis.all() or not col_vis.any():
            continue
        # Each hidden run takes a copy of the stretch of ring just before it, the same length: the hand-drawn wobble
        # carries on instead of one pixel smeared across the gap.
        col = polar[:, c].copy(); have = col_vis.copy()
        start = int(np.flatnonzero(col_vis)[0])
        i = 0
        while i < A:
            r = (start + i) % A
            if have[r]:
                i += 1
                continue
            n = 0
            while n < A and not have[(r + n) % A]:
                n += 1
            for k in range(n):
                src = (r + k - n) % A
                while not have[src]:
                    src = (src - n) % A
                col[(r + k) % A] = col[src]
            for k in range(n):
                have[(r + k) % A] = True
            i += n
        polar[:, c] = col
    back = cv2.warpPolar(polar, (2 * R, 2 * R), centre, R, flags | cv2.WARP_INVERSE_MAP)
    out = crop.copy()
    out[~seen] = back[~seen]
    return out


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, out = Path(argv[1]), Path(argv[2])
    im = cv2.imread(str(src), cv2.IMREAD_GRAYSCALE)
    if im is None:
        sys.exit(f'cannot read {src}')
    x0, y0 = ORIGIN
    if im.shape[0] < y0 + PERIOD or im.shape[1] < x0 + PERIOD:
        sys.exit(f'{src}: expected the {PERIOD * 2}-pixel picture of the pattern')
    tile = im[y0:y0 + PERIOD, x0:x0 + PERIOD].astype(np.float32)
    # enlarge with the neighbours around it, so the edges of the tile wrap
    pad = np.tile(tile, (3, 3))
    big = cv2.resize(pad, None, fx=SCALE, fy=SCALE, interpolation=cv2.INTER_CUBIC)
    big = cv2.GaussianBlur(big, (0, 0), SCALE * 0.35)
    rgb = two_tone(big)                                   # 3 x 3 tiles, two-tone
    N = PERIOD * SCALE
    out.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out / 'tile.png'), cv2.cvtColor(rgb[N:2 * N, N:2 * N], cv2.COLOR_RGB2BGR))
    front = [(cx, cy, r) for _, cx, cy, r, behind in CIRCLES if not behind]
    meta = {'period': PERIOD, 'scale': SCALE, 'ground': '#%02x%02x%02x' % DARK, 'stripes': 'stripes.png', 'circles': []}
    cv2.imwrite(str(out / 'stripes.png'), cv2.cvtColor(stripes(N), cv2.COLOR_RGB2BGR))
    # each line on its own (the line colour, transparent round it), so every line can flow up or down at its own pace
    lines = []
    for k, (c0, cov) in enumerate(stripe_lines(N)):
        img = np.zeros(cov.shape + (4,), np.uint8)
        img[..., :3] = LINE[::-1]
        img[..., 3] = (cov * 255).round().astype(np.uint8)
        cv2.imwrite(str(out / f'line-{k:02d}.png'), img)
        lines.append({'x': c0 / SCALE, 'width': cov.shape[1] / SCALE, 'file': f'line-{k:02d}.png'})
    meta['lines'] = lines
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)
    for name, cx, cy, r, behind in CIRCLES:
        # the whole circle with its black outline, cut along the outline's own jagged outer edge (not a perfect circle):
        # it lies on the stripes alone, as in the picture
        R = int(np.ceil((r + EDGE_OUT + 1) * SCALE))
        ccx, ccy = (N + cx * SCALE), (N + cy * SCALE)
        x, y = int(round(ccx)) - R, int(round(ccy)) - R
        crop = rgb[y:y + 2 * R, x:x + 2 * R]
        yy, xx = np.mgrid[0:2 * R, 0:2 * R].astype(np.float32)
        px, py = xx + x + 0.5, yy + y + 0.5               # pixel centres in the 3 x 3 image
        dist = np.hypot(px - ccx, py - ccy)
        others = [(N + (fx + ox) * SCALE, N + (fy + oy) * SCALE, fr) for fx, fy, fr in front
                  for ox in (-PERIOD, 0, PERIOD) for oy in (-PERIOD, 0, PERIOD)] if behind else []
        A = 2048
        edge = traced_edge(gray, ccx, ccy, r, others, A)
        k = np.round(np.arctan2(py - ccy, px - ccx) / (2 * np.pi) * A).astype(int) % A
        alpha = np.clip(edge[k] + 0.5 - dist, 0, 1)      # inside the traced edge, anti-aliased
        if behind:
            # The circles in front hide part of it. Turned, those parts would come into view, so they are filled with its
            # own rings: each hidden pixel takes the colour at the same radius from the nearest angle where it shows.
            seen = np.ones_like(alpha)
            for fx, fy, fr in front:
                for ox in (-PERIOD, 0, PERIOD):
                    for oy in (-PERIOD, 0, PERIOD):
                        d = np.hypot(px - (N + (fx + ox) * SCALE), py - (N + (fy + oy) * SCALE))
                        seen = np.minimum(seen, np.clip(d - (fr * SCALE + 2), 0, 1))
            crop = fill_hidden(crop, seen > 0.99, R)
        rgba = np.dstack([crop, (alpha * 255).round().astype(np.uint8)])
        cv2.imwrite(str(out / f'circle-{name}.png'), cv2.cvtColor(rgba, cv2.COLOR_RGBA2BGRA))
        meta['circles'].append({'name': name, 'x': cx, 'y': cy, 'r': r, 'size': 2 * R / SCALE,
                                'file': f'circle-{name}.png', 'behind': behind})
    (out / 'pattern.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(f'{out}: tile.png ({N} px), {len(CIRCLES)} circles')


if __name__ == '__main__':
    main(sys.argv)
