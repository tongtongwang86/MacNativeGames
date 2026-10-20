# Animal Crossing: New Horizons 2.0.6 (native, Apple silicon + Metal 4)

Runs ACNH 2.0.6 on macOS the same way `games/totk` runs TotK: the game's own ARM64 code executes natively, after
d2r's `patch_nso.py` rewrites the instructions that need the host (`x18`, `tpidr*`, `svc`, counter reads). Each
rewritten site is checked in an emulator against the original. NVN goes to Metal 4 through `d2r_gfx` and Ryujinx's
shader translator, shared with TotK through `third_party` → `../totk/third_party`.

You need your own dump. Nothing made from the game is ever committed: `extracted/`, `rewritten/`, `analysis/`,
`build/`, `user-data/` and `.venv` are in `.gitignore`.

## Status

| Step | State |
|---|---|
| N0: project, rewrite, build | Done. All three modules (`main`, `subsdk0`, `sdk`) rewrite with a structural check OK and 0 divergent sites in the emulator. `make build/acnh-native` builds. |
| N1: tables, imports, domains, `nnMain` | Done. `make verify-startup` passes: sdk, subsdk0 and main DT_INITs run (main has 8,090 constructors), and `nnMain` is entered. CMIF domain sessions work and are used by `nifm:u`. |
| N2: services | Main loop runs: about 60 frames a second for 120 s with no stop. Ported or added: `nifm:u`, `bcat:u` and `friend:u` (offline), hid touch and Joy-Con assignment, `nn::oe` region, device ID and exit section, settings language codes, time-zone rules and clocks, account (`OpenUser`, user list, profile image, no NSA), device save data (`MountDeviceSaveData`, sizes, `Commit`), RomFS directory listing, shared fonts (Hiragino substitute), ngc profanity filter. Audio renderer REV8 is running. |
| N3: graphics | **Title screen renders** with the imported island and resident ("Press A", Ver. 2.0.6) and runs steadily (100 s, about 4,300 frames). Fixes: NVN API 53.313; NVN's default depth mode (near = -w), which ACNH relies on (`-DNVN_DEFAULT_DEPTH_NEAR_MINUS_W=1`; without it the final copy to the window was clipped and the window stayed magenta). `make boot` turns GPU submission on with the TotK app's flags. Services added on the way: `nn::olsc` offline, an `fsp-srv` access-log query, `nn::account::ListOpenUsers`. **Open:** an intermittent NULL dereference in game UI code (`main+0x28d9248`), seen during the account-link dialogs; it did not reproduce under a debugger. |

The plan, and the list of what ACNH needs (services, applets, fonts, NVN 53), is in acnh-recomp's
`docs/NATIVE-EVALUATION.md`.

## Layout

- `extracted` → `../../../extracted`: `exefs/` (`main`, `subsdk0`, `sdk`) and `romfs/` from the base game plus the
  2.0.6 update.
- `scripts/validate_rewrites.py` rewrites the modules and runs `structck.py` and `equiv.py`. It writes
  `analysis/rewrite-validation.json` with each image's SHA-256. The only accepted mutation misses are `nop prfm`
  (prefetch hints).
- `scripts/prepare_runtime.py` reads that report and writes `build/runtime_tables.h`: relocations, symbols, gates
  and DT_INIT constructor flags.
- `runtime/` is forked from `games/totk/runtime`. The ACNH changes:
  - `acnh_patches.c`: the aspect-ratio patch (below) and `ACNH_SITE_PROBE`; TotK's frame-step and resolution hooks
    report "unavailable".
  - `romfs.c`: a read-only backend on the extracted romfs folder (`ACNH_ROMFS` overrides it). Traversal and links
    are rejected.
  - `ipc.c`: CMIF domains. ConvertCurrentObjectToDomain, domain requests and closes, and input objects.
    Interfaces a service returns as move handles become domain objects automatically.
  - `nifm.c`: no internet connection.
  - `loader.c`: constructors run in rtld order (sdk, subsdk0, main), and `TOTK_VERIFY_STARTUP` stops at `nnMain`.
  - The TotK-only parts were removed: version patches, world survey, the iOS frontend and the software keyboard
    probe.
- `scripts/import_save.py <saves dir> --uid <resident UID>` copies a Ryujinx/JKSV-layout device save (`0/`) into
  `user-data/save/device/` (it backs up an existing one first) and sets the account to the resident's UID. The source is
  only read.

## Screen shape

The picture follows the window's shape (`aspect_patch.h`, as in TotK): key 4 switches back to the game's 16:9 and
`TOTK_ASPECT=21:9` fixes a ratio. ACNH builds most of its sead perspective projections on the fly, so the ratio is put in
where sead reads it (`acnh_patches.c`), and only for projections whose aspect is exactly 16:9; the game's UI goes through
one of those too. Wider than 16:9 the view widens and the UI stays in the middle 16:9; narrower (16:10, 4:3, iPad) the view
keeps its width and grows taller, so nothing is cut off.

## 60 FPS

Not available. ACNH steps its simulation once per frame and asks for 30 fps (present interval 2, set once at start from
sead's framework config, main+0x26f7cd8); there is no time step to scale as in TotK, whose engine already supports a
variable frame rate. Its movement and animation use per-frame constants throughout (`mSpeedChaseStep`,
`mStickSpeedAddCalcRate`, ...), so running at 60 Hz plays the game at twice the speed. MetalFX frame interpolation (key 2)
does not engage yet: its scene, camera, depth and UI inputs are found with TotK-specific hooks.

## iPhone / iPad

```bash
python3 ios/tools/build_ipa.py   # after make build/acnh-native and a Mac run; writes build/ACNH.ipa (unsigned)
```

The same app as `games/totk/ios` (UIScene app, `runtime/frontend_ios.m`, touch controls). iOS can neither patch code nor
translate shaders at run time: the Mac binary exports its patched code (`export-ios`), and the shaders the Mac has
translated so far (`build/shader-cache`) become the app's seed, so play on the Mac where you want to play on the iPad
first, or shaders the Mac has not seen draw nothing on the device. The island is in the app's Documents
(`save/save/device`, Files app). SaveSync keeps it in step with the Mac only when the signing profile has the iCloud
entitlements (`libs/savesync/ARCHITECTURE.md`); the app carries the increased-memory-limit entitlement.

## Use

```bash
make dependencies      # Python 3.12 venv: lz4, capstone, unicorn
make rewrite           # rewrite + verify (about 90 s); LLVM_BIN defaults to /opt/homebrew/opt/llvm@21/bin
make tables
make build/acnh-native
make shader-tools     # SPIRV-Cross (pinned commit, built with clang) + the prebuilt shader translator
build/acnh-native rewritten probe   # map modules, bind imports, native cross-module calls
make verify-startup    # headless, exits at nnMain with a JSON line
make boot              # windowed, goes as far as the services allow
```

`TOTK_IPC_TRACE=1` logs every IPC request, and `TOTK_FS_LOG=1` logs every RomFS open.
