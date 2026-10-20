# SaveSync architecture

SaveSync keeps a native game port's save folder in step between a Mac and an iPhone or iPad through iCloud. It is two
files (`savesync.h`, `savesync.m`) with no dependencies beyond Foundation and CommonCrypto, and it knows nothing about
any game: a port describes where its saves are and calls two functions. Tears of the Kingdom (`games/totk`) is the
first user; any port with a host file system (the `fs_host.c` of this framework) can use it in the same way.

```
 Mac                                   iCloud                                  iPhone / iPad
 ┌──────────────────────────┐          ┌───────────────────────────────┐       ┌──────────────────────────┐
 │ game (fs_host.c)         │          │ container iCloud.<bundle id>  │       │ game (fs_host.c)         │
 │   savesync_start ───────┐│          │  Documents/SaveSync/          │       │┌────── savesync_start    │
 │   savesync_committed ──┐││  files   │   <title id>/                 │ files ││┌───── savesync_committed│
 │ profile/save/<account> │││ ◄──────► │     manifest.json  (last)     │ ◄───► │││      sceneDidEnterBack- │
 │ profile/savesync-*.json│││  (iCloud │     files/…        (first)    │(iCloud│││        ground → flush   │
 │ profile/save-backups/  │││   Drive) │                               │ dmn)  │││ Documents/save/…        │
 └────────────────────────┘││          └───────────────────────────────┘       │└┘ Documents/savesync-*   │
   ~/Library/Mobile Documents/                                               └──────────────────────────┘
   iCloud~<bundle~id>/… (no entitlement needed: iCloud Drive mirrors it)        URLForUbiquityContainerIdentifier
```

## The API

```c
savesync_config c = {
    .title_id     = "0100F2C0115B6000",   // the folder in iCloud: one per game, so games never meet
    .game_version = "1.4.3",              // a save from a newer version is never taken
    .save_dir     = save_root,            // whose *contents* are synced
    .state_dir    = profile,              // this device's state and save-backups/ (not synced)
    .container_id = NULL,                 // NULL: the app's own container (iOS) / any that holds the title (Mac)
    .log_tag      = "[totk]",
    .save_busy    = runtime_fs_save_busy, // uploads wait while the game is writing
    .keep_backups = 0,                    // 0: 10
};
savesync_start(&c);      // once, off the main thread, before the game reads its saves (blocking)
savesync_committed();    // after every save the game commits
savesync_flush(20);      // optional: app going to the background (also runs at exit, 10 s)
```

`savesync_start` returns 0 and everything else does nothing when there is no iCloud (no entitlement, signed out,
`SAVESYNC=0`, no container on the Mac), so a port calls it unconditionally.

## Layers (savesync.m, bottom to top)

| Layer | What it does | Platform differences |
|---|---|---|
| **1. sets** | A folder as `{relative path: bytes}` (hidden files skipped), its hash, writing a set back (unchanged files untouched, the rest atomically, stale files removed), backups with rotation, this device's state file | none |
| **2. store** | Finds the cloud folder; reads it (manifest, then the files it lists, checked against its hash); writes it (files first, manifest last) | iOS: ubiquity container, `startDownloadingUbiquitousItemAtURL`, an `NSMetadataQuery` that waits for iCloud's metadata, writes under `NSFileCoordinator`. Mac: plain files in `~/Library/Mobile Documents` (reading a dataless file fetches it) |
| **3. decision** | `savesync_decide`: a pure function from facts to an action | none |
| **4. scheduler** | One serial queue: the first sync, an upload 3 s after the last commit once `save_busy` says the game is done, flushes, the `atexit` flush | none |

## Data

**Cloud** (`<container>/Documents/SaveSync/<title id>/`):

```
manifest.json   {"format":1, "title":"0100F2C0115B6000", "hash":"<sha256>", "files":["option.sav","slot_00/progress.sav",…],
                 "bytes":…, "version":"1.4.3", "time":1792481584.8, "device":"tongtong-ipad-pro.local"}
files/          the save folder's contents
```

The **hash** is SHA-256 over every file's (path, NUL, size, bytes) in path order, so the same saves hash the same on
every device whatever the save folder is called (each device has its own account folder name). The manifest is
written **last** and a reader only uses files that hash to it, so a copy iCloud has only partly delivered is never
taken: it reads as "still arriving".

**Device state** (`<state_dir>/savesync-<title id>.json`): `{"last": "<hash>"}`, the hash this device and the cloud
last agreed on. It is what turns two snapshots into a three-way comparison.

**Backups** (`<state_dir>/save-backups/<time>-before-icloud` and `…-icloud-older`): whatever a sync replaces or
outvotes, the newest 10 kept.

## The decision

At start (the game has not read its saves yet), with L = this device's hash, C = the cloud's, A = last agreed:

| Situation | Action |
|---|---|
| no cloud copy | send (nothing, if this device has no saves either) |
| cloud copy still arriving | wait: keep this device's for now |
| L = C | in step |
| cloud from a newer game version | refuse (an older game cannot read it) |
| no saves here | take |
| only the cloud changed (L = A) | take |
| only this device changed (C = A) | send |
| both changed (or never agreed) | the newer wins (manifest time vs. newest local file); the other goes to save-backups |

During play only uploads happen, and only for **send without conflict**: if another device changed the cloud while
this one was playing, nothing is overwritten; the next start settles it by the table above (the newer wins, the other
is backed up). An empty save folder is never sent.

## Threads and timing

- `savesync_start` blocks its caller: up to 20 s for iCloud's metadata on iOS (only when the manifest is not already
  local), 30 s for the manifest, 60 s per listed file. It must not run on the main thread (the metadata query needs
  the main run loop; on the main thread it is skipped).
- Everything after runs on the library's serial queue: commits only bump a generation counter, the upload runs 3 s
  after the last commit and waits again while `save_busy()` is true, so a game that writes a save as several files and
  then commits is copied as one consistent snapshot (read into memory, hashed from memory).
- `savesync_flush` uploads immediately; the iOS app calls it from `sceneDidEnterBackground` inside a background task,
  and an `atexit` handler calls it on a normal Mac quit.

## Signing and setup

| | iPhone / iPad | Mac |
|---|---|---|
| Entitlements | `com.apple.developer.icloud-container-identifiers`, `ubiquity-container-identifiers` = `iCloud.<id>`, `icloud-services` = `CloudDocuments`, in a provisioning profile with the iCloud capability | none |
| Finding the folder | the app's container (or `container_id`) | `~/Library/Mobile Documents/iCloud~<id>/Documents/SaveSync/<title>`; set `container_id` or `SAVESYNC_CONTAINER` to create it from the Mac, otherwise the first iOS run creates it |
| Permission | none | macOS may ask once to let the game read iCloud Drive files |

Several games can share one container (one entitlement for all apps; each game has its own `SaveSync/<title id>`) or
each app can have its own: the Mac finds the title in whichever container holds it.

Environment: `SAVESYNC=0` turns it off, `SAVESYNC_DIR=<folder>` uses that folder as the title's cloud folder (tests,
another sync tool), `SAVESYNC_CONTAINER=iCloud.<id>` picks the container.

## Adding a game

1. Link the library into the game folder (the build copies links that point outside it as files):
   `ln -s ../../../libs/savesync games/<game>/vendor/savesync`
   (in another repository: copy `libs/savesync`, or keep it as a git subtree).
2. Mac build: add `vendor/savesync/savesync.m` to the binary's sources (Foundation is already linked).
   iOS build: add `../vendor/savesync` to the target's sources, excluding `tests/**` and `*.md`.
3. In the host file system, after the save folder exists and before the game opens a save:
   `savesync_start(&(savesync_config){.title_id = …, .game_version = …, .save_dir = …, .state_dir = …, .log_tag = …, .save_busy = …});`
   and after a save-data commit: `savesync_committed();`
4. iOS app: `savesync_flush` from `sceneDidEnterBackground` (see `games/totk/ios/Sources/main.m`).
5. Signing: the iCloud entitlements above on the iOS app.

## Limits

- Whole-folder snapshots: fine for save folders of a few tens of MB (TOTK: 32 files); not for gigabytes.
- Both devices playing at once is settled by time, file-set granularity: the newer set wins, the older is kept as a
  backup, slots are not merged.
- Times come from each device's clock; a clock far off can pick the wrong winner (the loser is still backed up).
- TOTK's first version kept its copy in `Documents/TOTK Saves`; SaveSync uses `Documents/SaveSync/0100F2C0115B6000`,
  starts from the device that next saves, and leaves the old folder untouched (safe to delete).

## Tests

`libs/savesync/tests/run.sh`: the decision table, then two simulated devices sharing a temporary cloud folder through
every case above (first device, empty device, in step, upload during play, conflict, an upload during play racing
another device's, newer game version, `SAVESYNC=0`). No game data or iCloud account needed.
