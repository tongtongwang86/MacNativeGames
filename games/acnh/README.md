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
| N3: graphics | NVN reports API 53.313 (`-DNVN_API_MAJOR/MINOR`; TotK keeps 55.15), and agl's device setup passes. Shaders translate (`make shader-tools`). **No picture yet:** the game runs with GPU submission off, the runtime's default since full-game runs panicked macOS upstream. The TotK app turns it on (`TOTK_GPU_ALLOW_SUBMIT=1` with `TOTK_GPU_WIDEN`, `BLOCKRT`, `MIDCLEAR` and `TOTK_GPU_BUDGET_MB=5000`). |

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
  - `acnh_patches.c`: there are no game-code patches yet, and TotK's frame-step, resolution and aspect hooks report
    "unavailable".
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
