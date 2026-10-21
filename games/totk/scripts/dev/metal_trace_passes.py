#!/usr/bin/env python3
"""Per-pass GPU time of a running totk-native from a Metal System Trace.

    metal_trace_passes.py <pid> <out.xml> [seconds] [top]   record (xctrace) + export + report
    metal_trace_passes.py <exported.xml> [top]              report an existing export

Start the game with DM_NVN_ENC_LABEL=1 so render encoders are named "pNN WxH <first fs key> dN" (the render stream is off
then; GPU times stay comparable). Rows: Vertex / Fragment / Compute channel time of all encoders with that target size and
first fragment shader, per game frame (frames = encoders of the final 1920x1080 composite, fs 309199f1). The runtime's
GPU keep-alive spin kernels (its own queue: "Command Buffer N:Compute Command 0") and WindowServer are reported apart:
the keep-alive fills idle GPU time, so Activity Monitor's GPU % says little about the game's own GPU load.
The runtime's TOTK_PASS_GPU timer is not usable for this on Apple GPUs (its stamps do not wait for earlier encoders).
"""
import collections, os, pickle, re, subprocess, sys, xml.etree.ElementTree as ET

def record(pid, out, secs):
    tr = out + '.trace'
    subprocess.run(['rm', '-rf', tr])
    subprocess.run(['xcrun', 'xctrace', 'record', '--template', 'Metal System Trace', '--attach', str(pid),
                    '--time-limit', '%ss' % secs, '--output', tr], check=True, stdout=subprocess.DEVNULL)
    with open(out, 'w') as f:
        subprocess.run(['xcrun', 'xctrace', 'export', '--input', tr, '--xpath',
                        '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]'], check=True, stdout=f)
    subprocess.run(['rm', '-rf', tr])

def load(path):
    pk = path + '.pkl'
    if os.path.exists(pk): return pickle.load(open(pk, 'rb'))
    vals, cols, rows = {}, None, []
    def val(e):
        r = e.get('ref')
        if r is not None: return vals.get(r)
        v = e.get('fmt') if e.get('fmt') is not None else (e.text or '')
        if e.tag in ('start-time', 'duration', 'uint64', 'metal-command-buffer-id', 'gpu-frame-number'): v = int(e.text) if e.text else 0
        if e.get('id') is not None: vals[e.get('id')] = v
        return v
    for _, e in ET.iterparse(path, events=('end',)):
        if e.tag == 'schema': cols = [c.findtext('mnemonic') for c in e.findall('col')]
        if e.tag == 'row':
            out = {}
            for c, ch in zip(cols, list(e)):
                for sub in ch.iter():
                    if sub is not ch and sub.get('id') is not None and sub.get('ref') is None: val(sub)
                out[c] = val(ch) if ch.tag != 'sentinel' else None
            rows.append((out['start'], out['duration'], out['channel-name'], (out.get('channel-subtitle') or '') or (out.get('event-label') or ''), out.get('encoder-id')))
            e.clear()
    pickle.dump(rows, open(pk, 'wb'))
    return rows

def report(rows, top):
    game, ka, ws, final = [], 0, 0, set()
    for r in rows:
        l = r[3]
        if 'WindowServer' in l: ws += r[1]; continue
        if re.match(r'Command Buffer \d+:Compute Command', l): ka += r[1]; continue
        m = re.search(r'p\d+ (\d+x\d+ \S+) d\d+', l)
        name = m.group(1) if m else '(unlabeled %s)' % r[2].lower()
        game.append((r, name))
        if name == '1920x1080 309199f1': final.add(r[4])
    nf = len(final) or 1
    iv = sorted((r[0], r[0] + r[1]) for r, _ in game); busy = 0; cs, ce = iv[0]
    for s, e in iv[1:]:
        if s > ce: busy += ce - cs; cs, ce = s, e
        else: ce = max(ce, e)
    busy += ce - cs; span = (iv[-1][1] - iv[0][0]) / 1e6
    print('trace %.0f ms, %d frames (%.1f fps)%s' % (span, nf, nf / span * 1e3, '' if final else ' -- no DM_NVN_ENC_LABEL labels: per-frame numbers are per trace'))
    print('game GPU busy %.3f ms/frame (%.0f%% of wall); keep-alive spin %.3f ms/frame; WindowServer %.3f ms/frame' % (busy / 1e6 / nf, 100 * busy / 1e6 / span, ka / 1e6 / nf, ws / 1e6 / nf))
    agg = collections.defaultdict(collections.Counter); chs = collections.Counter()
    for r, name in game: agg[name][r[2]] += r[1]; chs[r[2]] += r[1]
    print('game channel sums ms/frame: ' + ', '.join('%s %.3f' % (c, v / 1e6 / nf) for c, v in chs.most_common()))
    print('%-32s %7s %7s %7s %7s' % ('target + first fs', 'total', 'frag', 'vert', 'comp'))
    for name, c in sorted(agg.items(), key=lambda x: -sum(x[1].values()))[:top]:
        print('%-32s %7.3f %7.3f %7.3f %7.3f' % (name[:32], sum(c.values()) / 1e6 / nf, c['Fragment'] / 1e6 / nf, c['Vertex'] / 1e6 / nf, c['Compute'] / 1e6 / nf))

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    if a[0].isdigit():
        record(int(a[0]), a[1], a[2] if len(a) > 2 else 2); report(load(a[1]), int(a[3]) if len(a) > 3 else 40)
    else: report(load(a[0]), int(a[1]) if len(a) > 1 else 40)
