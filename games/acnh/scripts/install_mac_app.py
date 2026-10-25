#!/usr/bin/env python3
"""Install a Mac app that runs this checkout's ACNH: ~/Applications/Animal Crossing.app by default.

    python3 scripts/install_mac_app.py [--dest DIR] [--name NAME] [--no-build] [--self-contained]
                                       [--icloud-container iCloud.<id>] [--profile FILE]

It first builds what the game needs (make build/acnh-native romfs-coop shader-tools, and icon when tooling/AppIcon.icon
is there; --no-build skips that; scripts/speedfix.py for 60 FPS stays a separate step). The app holds the launcher, the
icon and its own copy of the game (Contents/MacOS/acnh-native), so rebuilding in this folder does not change the
installed app until it is installed again; the game's data stays in this folder (games/acnh: rewritten/, extracted/,
tooling/, the shader tools and caches and the local play overlay in build/, and the saves in user-data/).
--self-contained: the app holds the game's data too (about 11 GB: rewritten/, extracted/romfs and aoc, the 60 FPS
patch, the local play overlay, the shader tools) and runs without this folder; what the game writes goes to
~/Library/Application Support/<name>/ (user-data/: the saves, which the first install copies from this folder's
user-data/ when that folder is not there yet; shader-cache/: likewise copied from build/shader-cache). The game slot
(game_slot.c) stays this checkout's analysis/game-slot while that exists, so the app and the test games still take
turns, else it is in that folder too.
The launcher changes to this folder (self-contained: the app's Contents/Resources/game), sets what make boot sets (GPU submission flags), turns on local play's shared
editing (ACNH_MP_EDIT: every group, with build/romfs-coop's scripts) unless the environment sets it, names the iCloud container
SaveSync should use when given (SAVESYNC_CONTAINER: on a Mac that has not seen the container yet this creates it, so
the Mac's saves go up first), logs to ~/Library/Logs/MacNativeGames/acnh.log (acnh.previous.log: the run before), and
replaces itself with the game (exec: the same process, so the Dock keeps the app's name and icon).

macOS syncs an iCloud container only while an installed app is signed with it: otherwise iCloud Drive marks it
"blocked-app-uninstalled" (`brctl status iCloud.<id>`), nothing reaches the Mac and the container's local folder goes
away, so SaveSync stays off. --profile names a Mac provisioning profile for the iPhone / iPad app's App ID that
includes the container (Xcode makes one: a macOS app target with that bundle ID, iCloud Documents with the container,
automatic signing; it is the built app's Contents/embedded.provisionprofile). The app then takes that bundle ID, embeds
the profile and is signed with the container entitlements by --sign (default: the Apple Development certificate)."""
import argparse, os, plistlib, shutil, subprocess, sys, tempfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]

LAUNCHER = r'''
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void mkdirs(const char *path) {   /* mkdir -p */
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
int main(void) {
    const char *home = getenv("HOME");
    char game[PATH_MAX], bin[PATH_MAX], icon[PATH_MAX], exe[PATH_MAX], real[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) { fprintf(stderr, "ACNH: cannot find the app\n"); return 1; }
    char *slash = strrchr(real, '/'); *slash = 0;                 /* .../Contents/MacOS */
    snprintf(bin, sizeof bin, "%s/acnh-native", real);
    slash = strrchr(real, '/'); *slash = 0;                       /* .../Contents */
    snprintf(icon, sizeof icon, "%s/Resources/AppIcon.icns", real);
#if SELF_CONTAINED
    snprintf(game, sizeof game, "%s/Resources/game", real);
    char data[PATH_MAX], path[PATH_MAX];
    snprintf(data, sizeof data, "%s/Library/Application Support/%s", home ? home : "/tmp", APP_NAME);
    snprintf(path, sizeof path, "%s/user-data", data); mkdirs(path); setenv("TOTK_PROFILE_DIR", path, 0);
    snprintf(path, sizeof path, "%s/user-data/fonts", data); setenv("ACNH_SHARED_FONTS", path, 0);
    snprintf(path, sizeof path, "%s/shader-cache", data); mkdirs(path); setenv("TOTK_SHADER_CACHE", path, 0);
    setenv("TOTK_ROOT_DIR", game, 0);   /* the shader tools in build/ */
    struct stat st;
    if (stat(CHECKOUT_SLOT, &st) == 0 && S_ISDIR(st.st_mode)) setenv("TOTK_GAME_SLOT_DIR", CHECKOUT_SLOT, 0);
    else { snprintf(path, sizeof path, "%s/game-slot", data); setenv("TOTK_GAME_SLOT_DIR", path, 0); }
#else
    snprintf(game, sizeof game, "%s", GAME_DIR);
    setenv("TOTK_GAME_SLOT_DIR", CHECKOUT_SLOT, 0);   /* not next to the executable, which is inside the app */
#endif
    if (chdir(game)) { fprintf(stderr, "ACNH: cannot open %s: %s\n", game, strerror(errno)); return 1; }
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1); setenv("TOTK_GPU_WIDEN", "1", 1); setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1); setenv("TOTK_GPU_BUDGET_MB", "5000", 0);
    setenv("TOTK_SLOT_OWNER", "user", 0); setenv("TOTK_SLOT_PURPOSE", "player", 0);
    if (*ICLOUD_CONTAINER) setenv("SAVESYNC_CONTAINER", ICLOUD_CONTAINER, 0);
    setenv("ACNH_ICON_ICNS", icon, 0);
    if (home) {
        char dir[2048], log[2100], prev[2100];
        snprintf(dir, sizeof dir, "%s/Library/Logs/MacNativeGames", home); mkdir(dir, 0755);
        snprintf(log, sizeof log, "%s/%s.log", dir, LOG_NAME); snprintf(prev, sizeof prev, "%s/%s.previous.log", dir, LOG_NAME);
        rename(log, prev);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    }
    setenv("ACNH_MP_EDIT", MP_EDIT, 0);   /* local play: the visitor may do what the host does (docs/LAN-COOP-DESIGN.md) */
    char *argv[] = {"acnh-native", "rewritten", "boot", NULL};
    execv(bin, argv);
    fprintf(stderr, "ACNH: cannot start %s: %s\n", bin, strerror(errno));
    return 1;
}
'''


MP_EDIT = 'furniture,visitor-grab,museum,visitor-turn,push-anim,house,decor-sync,nook,mail,hotel'


def c_string(s): return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def signing(profile, container):
    """(bundle ID, entitlements) for signing the app with the profile and the iCloud container."""
    data = plistlib.loads(subprocess.run(['security', 'cms', '-D', '-i', str(profile)], check=True, capture_output=True).stdout)
    allowed = data.get('Entitlements', {})
    app_id = allowed.get('com.apple.application-identifier', '')
    team = allowed.get('com.apple.developer.team-identifier', '')
    if not app_id.startswith(team + '.') or app_id.endswith('*'): raise SystemExit(f'{profile}: not a profile for one App ID')
    ent = {'com.apple.application-identifier': app_id, 'com.apple.developer.team-identifier': team}
    if container:
        if container not in allowed.get('com.apple.developer.icloud-container-identifiers', []):
            raise SystemExit(f'{profile}: does not include the iCloud container {container}')
        ent.update({'com.apple.developer.icloud-container-identifiers': [container],
                    'com.apple.developer.ubiquity-container-identifiers': [container],
                    'com.apple.developer.icloud-services': ['CloudDocuments']})
    if data.get('ProvisionedDevices'): ent['com.apple.security.get-task-allow'] = True   # a development profile
    return app_id[len(team) + 1:], ent


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--dest', type=Path, default=Path.home() / 'Applications')
    p.add_argument('--name', default='Animal Crossing')
    p.add_argument('--icloud-container', default='', help='e.g. iCloud.com.example.save (the one the iPhone / iPad app is signed with)')
    p.add_argument('--profile', type=Path, help='Mac provisioning profile with the container: the app is signed with it')
    p.add_argument('--sign', default='Apple Development', help='signing identity used with --profile')
    p.add_argument('--no-build', action='store_true', help='install what build/ already has')
    p.add_argument('--self-contained', action='store_true', help="put the game's data in the app (about 11 GB)")
    a = p.parse_args()
    if not a.no_build:
        targets = ['build/acnh-native', 'romfs-coop', 'shader-tools'] + (['icon'] if (GAME / 'tooling/AppIcon.icon').exists() else [])
        subprocess.run(['make', '-C', str(GAME)] + targets, check=True)
    bundle_id, entitlements = signing(a.profile, a.icloud_container) if a.profile else ('org.macnativegames.acnh', None)
    for need in ('build/acnh-native', 'rewritten', 'extracted/romfs', 'build/romfs-coop'):
        if not (GAME / need).exists(): raise SystemExit(f'missing {GAME / need}: build the game first (README "Use")')
    icns = GAME / 'build/AppIcon.icns'
    app = a.dest / f'{a.name}.app'
    a.dest.mkdir(parents=True, exist_ok=True)
    tmp = app.with_name(app.name + '.partial'); shutil.rmtree(tmp, ignore_errors=True)
    (tmp / 'Contents/MacOS').mkdir(parents=True); (tmp / 'Contents/Resources').mkdir()
    info = {'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleIdentifier': bundle_id,
            'CFBundleExecutable': 'ACNH', 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '3.0.0',
            'CFBundleVersion': '1', 'LSMinimumSystemVersion': '26.0', 'NSHighResolutionCapable': True,
            'LSApplicationCategoryType': 'public.app-category.games'}
    if icns.is_file():
        shutil.copy2(icns, tmp / 'Contents/Resources/AppIcon.icns'); info['CFBundleIconFile'] = 'AppIcon'
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    log_name = 'acnh' if a.name == 'Animal Crossing' else ''.join(c if c.isalnum() else '-' for c in a.name.lower())
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'launcher.c'; src.write_text(LAUNCHER)
        subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Werror', f'-DGAME_DIR={c_string(str(GAME))}',
                        f'-DICLOUD_CONTAINER={c_string(a.icloud_container)}', f'-DMP_EDIT={c_string(MP_EDIT)}',
                        f'-DSELF_CONTAINED={int(a.self_contained)}', f'-DAPP_NAME={c_string(a.name)}', f'-DLOG_NAME={c_string(log_name)}',
                        f'-DCHECKOUT_SLOT={c_string(str(GAME / "analysis/game-slot"))}',
                        str(src), '-o', str(tmp / 'Contents/MacOS/ACNH')], check=True)
    identity = a.sign if entitlements else '-'
    shutil.copy2(GAME / 'build/acnh-native', tmp / 'Contents/MacOS/acnh-native')   # the game itself, signed with the app below
    subprocess.run(['codesign', '--force', '--sign', identity, str(tmp / 'Contents/MacOS/acnh-native')], check=True, capture_output=True)
    if a.self_contained:
        game = tmp / 'Contents/Resources/game'
        parts = ['rewritten', 'extracted/romfs', 'extracted/aoc', 'tooling/acnh-60fps', 'build/romfs-coop',
                 'build/shader-service/totk_xlate', 'build/spirv-cross/spirv-cross']
        for part in parts:
            if not (GAME / part).exists(): print(f'(no {part}: left out)'); continue
            (game / part).parent.mkdir(parents=True, exist_ok=True)
            print(f'copying {part} ...', flush=True)
            subprocess.run(['ditto', str(GAME / part), str(game / part)], check=True)
        for tool in ('build/shader-service/totk_xlate', 'build/spirv-cross/spirv-cross'):   # helpers the game starts
            if (game / tool).exists():
                subprocess.run(['codesign', '--force', '--sign', identity, str(game / tool)], check=True, capture_output=True)
        data = Path.home() / 'Library/Application Support' / a.name
        if not (data / 'user-data').exists() and (GAME / 'user-data').exists():
            print(f'copying the saves to {data / "user-data"}', flush=True)
            data.mkdir(parents=True, exist_ok=True)
            subprocess.run(['ditto', str(GAME / 'user-data'), str(data / 'user-data')], check=True)
        caches = sorted((GAME / 'build/shader-cache').glob('rt-*'), key=lambda c: c.stat().st_mtime)
        if not (data / 'shader-cache').exists() and caches:
            print(f'copying the shader cache to {data / "shader-cache"}', flush=True)
            data.mkdir(parents=True, exist_ok=True)
            subprocess.run(['ditto', str(caches[-1]), str(data / 'shader-cache')], check=True)
        print('signing the app (it hashes every file) ...', flush=True)
    if entitlements:
        shutil.copy2(a.profile, tmp / 'Contents/embedded.provisionprofile')
        with tempfile.NamedTemporaryFile(suffix='.entitlements') as f:
            f.write(plistlib.dumps(entitlements)); f.flush()
            r = subprocess.run(['codesign', '--force', '--sign', a.sign, '--entitlements', f.name, '--generate-entitlement-der',
                                str(tmp)], capture_output=True, text=True)
        if r.returncode: sys.exit(f'codesign: {r.stderr.strip()}')
    else:
        subprocess.run(['codesign', '--force', '--sign', '-', str(tmp)], check=True, capture_output=True)
    shutil.rmtree(app, ignore_errors=True); tmp.rename(app)
    subprocess.run(['/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister', '-f', str(app)],
                   capture_output=True)
    print(f'installed {app} (game: ' + ('in the app' if a.self_contained else str(GAME)) + (f', iCloud container {a.icloud_container}' if a.icloud_container else '')
          + (f', signed as {bundle_id} with {a.profile.name}' if entitlements else '') + ')')


if __name__ == '__main__':
    main()
