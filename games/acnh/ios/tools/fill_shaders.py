#!/usr/bin/env python3
"""Translate the shader variants the iPad / iPhone was missing and send them back to it (ACNH).

iOS has no shader translator: on a miss the game skips the draws that need the variant (a dig hole that never shows, a
fishing splash, wood drawn black when only its colour pass is missing) and saves the request to
Documents/shader-requests/. This pulls the requests, translates them into the Mac cache (build/shader-cache/rt-<chain>,
so the next build_ipa.py seed carries them too), compiles device libraries and pushes one pack to Documents/shader-fill/;
the game merges it on its next start. TOTK's ios/tools/fill_shaders.py does the work; ACNH keeps no shared shader list.
usage: fill_shaders.py --device <id> [--bundle-id <id>] [--requests <dir>] [--with-done] [--no-push]"""
import argparse, shutil, subprocess, sys, tempfile, time
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME.parent / 'totk/ios/tools'))
import fill_shaders as totk   # noqa: E402


def main():
    ap = argparse.ArgumentParser(description='Translate the shaders the iPad missed and push them back to it.')
    ap.add_argument('--device', default=None, help='devicectl device id')
    ap.add_argument('--bundle-id', default='com.tongtonginc.acnh')
    ap.add_argument('--requests', type=Path, help='use requests already copied from the device instead of pulling them')
    ap.add_argument('--no-push', action='store_true', help='translate and build the pack, but do not send it')
    ap.add_argument('--with-done', action='store_true', help='also rebuild the variants filled before (shader-requests/done)')
    args = ap.parse_args()
    if not args.requests and not args.device: raise RuntimeError('give --device, or --requests')
    if not args.no_push and not args.device: raise RuntimeError('pushing needs --device; use --no-push')
    ch = totk.chain(); env = totk.xcode_env()
    out = GAME / f'build/shader-cache/rt-{ch}'; out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='acnh-shader-fill-'))
    try:
        folder = args.requests
        if not folder:
            folder = work / 'requests'
            r = totk.devicectl(env, args, 'from', 'Documents/shader-requests', folder)
            if r.returncode: print(f"pull was incomplete, using what arrived: {r.stderr.strip().splitlines()[-1:]}")
        rqs = totk.load_requests(folder)
        if args.with_done:
            done = folder / 'done'
            if not args.requests and not done.is_dir():
                r = totk.devicectl(env, args, 'from', 'Documents/shader-requests/done', done)
                if r.returncode: print(f"pull of done requests was incomplete: {r.stderr.strip().splitlines()[-1:]}")
            seen = {rq['variant'] for rq in rqs}
            rqs += [rq for rq in totk.load_requests(done) if rq['variant'] not in seen]
        if not rqs: print('no shader requests from the device'); return
        failed = totk.translate(rqs, GAME, out, ch)
        variants = sorted({rq['variant'] for rq in rqs} - failed)
        if not variants: raise RuntimeError('no variant could be translated')
        name = time.strftime('%Y%m%d-%H%M%S'); pack = work / name; pack.mkdir()
        count = totk.build_pack(pack, out, variants, ch, env)
        print(f'pack {name}: {len(variants)} variants, {count} files' + (f', {len(failed)} failed' if failed else ''))
        if args.no_push:
            keep = GAME / 'build/shader-fill' / name; keep.parent.mkdir(parents=True, exist_ok=True); shutil.move(str(pack), keep)
            print(f'not pushed; the pack is in {keep}'); return
        r = totk.devicectl(env, args, 'to', pack, f'Documents/shader-fill/{name}')
        if r.returncode: raise RuntimeError(f"push failed: {r.stderr.strip()[-400:]}")
        print('pushed; restart the game on the device to merge and warm the new shaders')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    try: main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e: print(f'ERROR: {e}', file=sys.stderr); sys.exit(1)
