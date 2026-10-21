#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone / iPad IPA of ACNH from this folder (games/acnh), after make rewrite, make tables,
make build/acnh-native and at least one Mac run (its translated shaders become the app's shader seed).

    python3 ios/tools/build_ipa.py [--out build/ACNH.ipa] [--app-name ...] [--bundle-id ...]

The same steps as games/totk/ios/tools/build_ipa.py, whose helpers it uses:
  1. iOS cannot write code at run time: the Mac binary applies the game patches (acnh_patches.c, aspect stubs) and
     exports the patched text (export-ios, TOTK_EXPORT_IOS); gen_embed.py puts it in executable segments of the app.
  2. opus and libvpx are built for the device (build/ios-deps).
  3. iOS cannot translate shaders either: the Mac's translated shaders (build/shader-cache/rt-<chain>) are the seed,
     compiled into device metallibs (compile_seed.py). Shaders the Mac has not seen yet are missing on the device until
     a Mac run has met them and the IPA is built again.
  4. xcodegen + xcodebuild (unsigned), the increased-memory-limit entitlement in an ad-hoc signature, the player's icon
     (--icon), then RomFS, the 60 FPS SpeedFix when tooling/acnh-60fps has it, the DLC folders, the seed and a system font
     into GameData/, zipped as Payload/ACNH.app.
Saves live in the app's Documents (Files app: save/save/device is the island); with an iCloud-capable signing profile
SaveSync keeps them in step with the Mac."""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]            # games/acnh
TOTK_TOOLS = GAME.parent / 'totk/ios/tools'
sys.path.insert(0, str(TOTK_TOOLS))
from compile_seed import METAL_FLAGS                  # noqa: E402
from xcode_dir import developer_dir                   # noqa: E402

SYSTEM_FONTS = ['/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc', '/System/Library/Fonts/Supplemental/Arial.ttf']


def run(args, cwd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.run([str(x) for x in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-35:]
        raise RuntimeError(f'{args[0]} failed; see {log}\n' + '\n'.join(tail))


def clone(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode: shutil.copy2(src, dst)


def copy_tree(src, dst):
    if dst.exists(): shutil.rmtree(dst)
    dst.mkdir(parents=True)
    if subprocess.run(['/bin/cp', '-cR', str(src) + os.sep + '.', str(dst)], capture_output=True).returncode:
        shutil.rmtree(dst); shutil.copytree(src, dst)


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def chain():
    text = (GAME.parent / 'totk/third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
    return text.split('#define DMSH_CHAIN "', 1)[1].split('"', 1)[0]


def write_shader_map(seed):
    """The runtime opens the seed's shader_map.tsv in place (DM_NVN_SHMAP) and checks its first line, the format's name
    (shared with TOTK); ACNH needs no entries in it. Written on every build, also when the device libraries are reused."""
    (seed / 'shader_map.tsv').write_text('# TOTK control-code-sha256-v1\n')


def make_seed(gen, logs, env):
    """The Mac's runtime shader cache as a seed (the files the importer accepts), compiled for the device."""
    ch = chain(); cache = GAME / f'build/shader-cache/rt-{ch}'
    if not cache.is_dir(): raise RuntimeError(f'no translated shaders yet ({cache}); run the game on the Mac first (make boot)')
    names = sorted(p.name for p in cache.iterdir() if p.is_file() and (p.name.endswith(('_b.metal', '_b.json', '.variants'))))
    shaders = sum(n.endswith('_b.metal') for n in names)
    source_hash = hashlib.sha256('\n'.join(f'{n} {sha(cache / n)}' for n in names).encode()).hexdigest()
    seed = gen / 'acnh/shader-seed'; manifest_path = seed / 'seed-manifest.json'
    if manifest_path.is_file():
        old = json.loads(manifest_path.read_text())
        if old.get('ios_source_sha256') == source_hash and old.get('ios_metal_flags') == ' '.join(METAL_FLAGS):
            write_shader_map(seed)
            print(f'shader seed: {shaders} shaders, device libraries up to date', flush=True)
            return seed, shaders
    if seed.exists(): shutil.rmtree(seed)
    seed.mkdir(parents=True)
    for n in names: clone(cache / n, seed / n)
    if (cache / 'pso_collected.tsv').is_file(): clone(cache / 'pso_collected.tsv', seed / 'pso_manifest.tsv')   # the pipelines the Mac met
    files = {p.name: sha(p) for p in sorted(seed.iterdir())}
    manifest_path.write_text(json.dumps({'chain': ch, 'schema': 1, 'shaders': shaders, 'files': files}, indent=2) + '\n')
    run([sys.executable, TOTK_TOOLS / 'compile_seed.py', seed, 'iphoneos'], GAME, logs / 'ios-shaders.log', env)
    m = json.loads(manifest_path.read_text()); m['ios_source_sha256'] = source_hash
    manifest_path.write_text(json.dumps(m, indent=2) + '\n')
    write_shader_map(seed)
    print(f'shader seed: {shaders} shaders from the Mac cache, compiled for the device', flush=True)
    return seed, shaders


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--out', type=Path, default=GAME / 'build/ACNH.ipa')
    p.add_argument('--logs', type=Path, default=GAME / 'build/ios-logs')
    p.add_argument('--app-name', default='Animal Crossing'); p.add_argument('--bundle-id', default='org.macnativegames.acnh')
    p.add_argument('--icon', type=Path, default=GAME / 'tooling/AppIcon.icon', help='an Icon Composer .icon package (default tooling/AppIcon.icon, if there)')
    a = p.parse_args(); ios = GAME / 'ios'; gen = ios / 'Gen'; logs = a.logs.resolve(); out = a.out.resolve()
    if out.suffix.lower() != '.ipa': raise RuntimeError('the output must end in .ipa')
    env = dict(os.environ, DEVELOPER_DIR=developer_dir())
    if not shutil.which('xcodegen'): raise RuntimeError('install xcodegen: brew install xcodegen')
    host = GAME / 'build/acnh-native'
    if not host.is_file(): raise RuntimeError('build the Mac binary first: make build/acnh-native')
    gen.mkdir(parents=True, exist_ok=True)
    # 1. patched code, exported by the Mac binary and embedded in executable segments
    modules = gen / 'modules'; modules.mkdir(exist_ok=True)
    run([host, GAME / 'rewritten', 'export-ios'], GAME, logs / 'export-ios.log', dict(env, TOTK_EXPORT_IOS=str(modules)))
    flags = subprocess.check_output([sys.executable, TOTK_TOOLS / 'gen_embed.py', GAME / 'rewritten', gen / 'embed.S', '--text-dir',
                                     modules, 'main', 'subsdk0', 'sdk'], text=True, env=env).strip()
    print('patched code exported', flush=True)
    # 2. device libraries (Homebrew's Mac archives cannot be linked into an iOS app)
    deps = GAME / 'build/ios-deps'
    if not (deps / 'lib/libopus.a').is_file() or not (deps / 'lib/libvpx.a').is_file():
        run([sys.executable, TOTK_TOOLS / 'build_deps.py', deps], GAME, logs / 'ios-dependencies.log', env)
    shutil.copytree(deps / 'include', gen / 'deps/include', dirs_exist_ok=True)
    for lib in ('libopus.a', 'libvpx.a'): clone(deps / 'lib' / lib, gen / 'deps/lib' / lib)
    # 3. shaders
    seed, _ = make_seed(gen, logs, env)
    # 4. the app
    env.update(ACNH_APP_NAME=a.app_name, ACNH_BUNDLE_ID=a.bundle_id)
    frameworks = ' '.join('-framework ' + x for x in ['UIKit', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore', 'ImageIO', 'CoreGraphics',
                                                      'CoreText', 'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox',
                                                      'GameController', 'CoreHaptics', 'CoreMotion', 'AVFAudio', 'Security'])
    (gen / 'acnh.xcconfig').write_text(f'OTHER_LDFLAGS = $(inherited) {frameworks} Gen/deps/lib/libopus.a Gen/deps/lib/libvpx.a {flags}\n')
    clone(ios / 'Sources/Info.plist', gen / 'Info.plist')   # xcodegen fills in the copy, the source stays as it is
    run(['xcodegen', 'generate', '--spec', 'project.yml'], ios, logs / 'xcodegen.log', env)
    derived = GAME / 'build/ios-derived'
    run(['xcodebuild', '-project', 'ACNHIOS.xcodeproj', '-scheme', 'ACNHIOS', '-configuration', 'Release', '-destination', 'generic/platform=iOS',
         '-derivedDataPath', derived, 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY='], ios, logs / 'xcodebuild-ios.log', env)
    app = derived / 'Build/Products/Release-iphoneos/ACNH.app'
    if not app.is_dir(): raise RuntimeError(f'Xcode did not produce {app}')
    # The entitlement in the executable's ad-hoc signature, so re-signing tools keep it.
    run(['codesign', '--force', '--sign', '-', '--entitlements', ios / 'Sources/ACNH.entitlements', app / 'ACNH'], ios, logs / 'ios-entitlements.log', env)
    if a.icon and a.icon.is_dir():   # the player's own icon: compiled into the app (asset catalog and icon files) and named in Info.plist
        import plistlib, tempfile
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'AppIcon.icon'; shutil.copytree(a.icon, src); partial = Path(d) / 'icon-info.plist'
            run(['xcrun', 'actool', src, '--compile', app, '--platform', 'iphoneos', '--minimum-deployment-target', '26.0', '--target-device', 'ipad',
                 '--target-device', 'iphone', '--app-icon', 'AppIcon', '--output-partial-info-plist', partial], ios, logs / 'icon-iphoneos.log', env)
            info = plistlib.loads((app / 'Info.plist').read_bytes()); info.update(plistlib.loads(partial.read_bytes()))
            (app / 'Info.plist').write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
        print(f'app icon: {a.icon}', flush=True)
    print('app built', flush=True)
    res = app / 'GameData'; res.mkdir(exist_ok=True)
    copy_tree((GAME / 'extracted/romfs').resolve(), res / 'romfs')
    copy_tree(seed, res / 'shader-seed')
    sf = GAME / 'tooling/acnh-60fps'   # 60 FPS SpeedFix (scripts/speedfix.py): its code is in the export above; the app gets
    if (sf / 'speedfix_main_300.pchtxt').is_file():   # the list for its data words and the RomFS overlay
        (res / 'speedfix').mkdir(exist_ok=True); clone(sf / 'speedfix_main_300.pchtxt', res / 'speedfix/speedfix_main_300.pchtxt')
        copy_tree(sf / 'romfs', res / 'speedfix/romfs'); print('60 FPS SpeedFix included', flush=True)
    aoc = (GAME / 'extracted/aoc').resolve()   # DLC folders (extracted/aoc/<index>), if any
    if aoc.is_dir():
        (res / 'aoc').mkdir(exist_ok=True)
        for d in sorted(aoc.iterdir()):
            if d.is_dir() and d.name.isdigit(): copy_tree(d, res / 'aoc' / d.name)
    uid = GAME / 'user-data/account.uid'   # the account the island's residents belong to (import_save.py --uid)
    if uid.is_file(): clone(uid, res / 'account.uid'); print('account: the Mac profile\'s', flush=True)
    fonts = GAME / 'user-data/fonts'
    if fonts.is_dir(): copy_tree(fonts, res / 'shared-fonts')   # a dump from the player's own console, if there is one
    font = next((Path(f) for f in SYSTEM_FONTS if Path(f).is_file()), None)
    if font: clone(font, res / ('font' + font.suffix))
    payload = GAME / 'build/ios-payload'; shutil.rmtree(payload, ignore_errors=True); (payload / 'Payload').mkdir(parents=True)
    copy_tree(app, payload / 'Payload/ACNH.app')
    out.parent.mkdir(parents=True, exist_ok=True); out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, logs / 'ipa-zip.log', env)
    shutil.rmtree(payload, ignore_errors=True)
    print(f'IPA ready: {out} ({out.stat().st_size / 2**30:.1f} GiB); unsigned, sideload with your signing tool')


if __name__ == '__main__':
    try: main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e: print(f'ERROR: {e}', file=sys.stderr); sys.exit(1)
