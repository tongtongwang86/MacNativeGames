#!/usr/bin/env python3
"""Install a Mac app that runs this checkout's ACNH: ~/Applications/Animal Crossing.app by default.

    python3 scripts/install_mac_app.py [--dest DIR] [--name NAME] [--icloud-container iCloud.<id>]

The app holds only a launcher and the icon; the game stays in this folder (games/acnh: build/acnh-native, rewritten/,
extracted/, tooling/, user-data/), so build it first (make build/acnh-native, make icon, scripts/speedfix.py for 60 FPS).
The launcher changes to this folder, sets what make boot sets (GPU submission flags), names the iCloud container
SaveSync should use when given (SAVESYNC_CONTAINER: on a Mac that has not seen the container yet this creates it, so
the Mac's saves go up first), logs to ~/Library/Logs/MacNativeGames/acnh.log (acnh.previous.log: the run before), and
replaces itself with the game (exec: the same process, so the Dock keeps the app's name and icon)."""
import argparse, os, plistlib, shutil, subprocess, tempfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]

LAUNCHER = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    const char *game = GAME_DIR, *home = getenv("HOME");
    if (chdir(game)) { fprintf(stderr, "ACNH: cannot open %s: %s\n", game, strerror(errno)); return 1; }
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1); setenv("TOTK_GPU_WIDEN", "1", 1); setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1); setenv("TOTK_GPU_BUDGET_MB", "5000", 0);
    setenv("TOTK_SLOT_OWNER", "user", 0); setenv("TOTK_SLOT_PURPOSE", "player", 0);
    if (*ICLOUD_CONTAINER) setenv("SAVESYNC_CONTAINER", ICLOUD_CONTAINER, 0);
    setenv("ACNH_ICON_ICNS", ICON_PATH, 0);
    if (home) {
        char dir[2048], log[2100], prev[2100];
        snprintf(dir, sizeof dir, "%s/Library/Logs/MacNativeGames", home); mkdir(dir, 0755);
        snprintf(log, sizeof log, "%s/acnh.log", dir); snprintf(prev, sizeof prev, "%s/acnh.previous.log", dir);
        rename(log, prev);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    }
    char *argv[] = {"acnh-native", "rewritten", "boot", NULL};
    execv("build/acnh-native", argv);
    fprintf(stderr, "ACNH: cannot start build/acnh-native: %s\n", strerror(errno));
    return 1;
}
'''


def c_string(s): return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--dest', type=Path, default=Path.home() / 'Applications')
    p.add_argument('--name', default='Animal Crossing')
    p.add_argument('--icloud-container', default='', help='e.g. iCloud.com.example.save (the one the iPhone / iPad app is signed with)')
    a = p.parse_args()
    for need in ('build/acnh-native', 'rewritten', 'extracted/romfs'):
        if not (GAME / need).exists(): raise SystemExit(f'missing {GAME / need}: build the game first (README "Use")')
    icns = GAME / 'build/AppIcon.icns'
    app = a.dest / f'{a.name}.app'
    a.dest.mkdir(parents=True, exist_ok=True)
    tmp = app.with_name(app.name + '.partial'); shutil.rmtree(tmp, ignore_errors=True)
    (tmp / 'Contents/MacOS').mkdir(parents=True); (tmp / 'Contents/Resources').mkdir()
    info = {'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleIdentifier': 'org.macnativegames.acnh',
            'CFBundleExecutable': 'ACNH', 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '3.0.0',
            'CFBundleVersion': '1', 'LSMinimumSystemVersion': '26.0', 'NSHighResolutionCapable': True,
            'LSApplicationCategoryType': 'public.app-category.games'}
    if icns.is_file():
        shutil.copy2(icns, tmp / 'Contents/Resources/AppIcon.icns'); info['CFBundleIconFile'] = 'AppIcon'
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'launcher.c'; src.write_text(LAUNCHER)
        subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Werror', f'-DGAME_DIR={c_string(str(GAME))}',
                        f'-DICLOUD_CONTAINER={c_string(a.icloud_container)}',
                        f'-DICON_PATH={c_string(str(app / "Contents/Resources/AppIcon.icns"))}',
                        str(src), '-o', str(tmp / 'Contents/MacOS/ACNH')], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(tmp)], check=True, capture_output=True)
    shutil.rmtree(app, ignore_errors=True); tmp.rename(app)
    subprocess.run(['/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister', '-f', str(app)],
                   capture_output=True)
    print(f'installed {app} (game: {GAME}' + (f', iCloud container {a.icloud_container}' if a.icloud_container else '') + ')')


if __name__ == '__main__':
    main()
