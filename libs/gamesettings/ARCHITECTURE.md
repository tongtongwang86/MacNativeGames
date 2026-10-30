# GameSettings architecture

One Settings screen for the native game ports: keyboard bindings shared by every game, and each game's graphics
settings, kept on each device (not synced: each has its own screen, controls and speed). On a Mac it is a window (⌘, from the app menu); on iPhone and iPad a
sheet (the gear between − and + on the touch controls, or ⌘, on a hardware keyboard). Like SaveSync it is two files
(`gamesettings.h`, `gamesettings.m`) plus the controller drawing (`procon_pdf.h`), and knows nothing about any game.

## Layers (gamesettings.m)

| Layer | What it does |
|---|---|
| 1. keyboard | The 24 Switch inputs (A … ZR, +/−, stick presses, d-pad, 4 directions per stick), their default keys, the bindings, and a per-key lookup (Switch button bits, stick directions) the game's input thread reads every frame without locking |
| 2. store | `Settings/keyboard.json` (every game) and `Settings/<title id>.json` (one game), on this device only, written at every change |
| 3. editing | Giving an input a key (a key does one thing: it leaves any other input), clearing, defaults, key names; iOS hardware keys (USB HID usages) mapped to the Mac key codes the bindings use |
| 4. drawing | Ryujinx's Pro Controller with a key box per control and a line to it, in Core Graphics, the same on Mac and iOS; hit-testing for boxes and controls |
| 5. windows | AppKit window (toolbar tabs Graphics / Controls / Saves in the System Settings look: grouped rounded boxes, Off / On rows as switches; a row with `controls` set sits on Controls above the keyboard bindings, one with `group` starts a heading; pop-ups, key recorder via a local event monitor) and UIKit sheet (segmented tabs, menus, key recorder via `pressesBegan`) |

## Using it in a port

```c
static const gamesettings_choice graphics[] = {
    {"picture", "Picture", picture_choices, 3, picture_get, picture_set, "Key 3.", 0, "Picture"},   // get/set on the main thread; "Picture": a group heading
    {"mouse", "Mouse", onoff_choices, 2, mouse_get, mouse_set, NULL, 1, "Controller"},  // 1: on the Controls tab
};
static const gamesettings_config config = {"0100F2C0115B6000", "Tears of the Kingdom", "[totk]", graphics, 1};

gamesettings_start(&config);                       // early, any thread
gamesettings_add_menu_item(app_menu);              // Mac: "Settings…  ⌘,"
gamesettings_apply_graphics();                     // once the renderer draws (TOTK: after 60 frames)
state.buttons |= gamesettings_buttons(user_keys);  // input thread, every frame
gamesettings_sticks(user_keys, &lx, &ly, &rx, &ry);
gamesettings_note_graphics_changed();              // a shortcut key changed a graphics setting
gamesettings_show();                               // iOS: from a button or ⌘,
```

Keys from scripts (auto-load, test input files) should keep their own fixed table, as TOTK's `input.c` does, so a
player's bindings never change what a script presses.

## Storage

```
Settings/keyboard.json          {"format":1, "time":…, "bindings": {"a":[38], "b":[40,49], …}}
Settings/0100F2C0115B6000.json  {"format":1, "time":…, "values": {"picture":1, "shape":0, …}}
```

`Settings/` is `~/Library/Application Support/MacNativeGames/Settings` (Mac) or the app's `Documents/Settings` (iOS);
`GAMESETTINGS_LOCAL_DIR` names another (tests). Nothing goes to iCloud: only the saves sync (SaveSync). Earlier
builds also wrote a copy to `<container>/Documents/Settings/` in iCloud; it is no longer read (safe to delete). Key codes are Mac virtual
key codes, i.e. positions on a US keyboard.

## The controller drawing

`assets/Controller_ProCon.svg` is Ryujinx's (MIT; `THIRD_PARTY_NOTICES.md`). `tools/make_procon_pdf.sh` turns it into a
vector PDF embedded as bytes in `procon_pdf.h`, so neither platform needs an SVG renderer or a resource bundle. Control
positions in `controls[]` are in the SVG's coordinates.

## Tests

`tests/run.sh`: default keys with nothing loaded, a saved binding, a copy in a cloud folder ignored,
a graphics choice applied. The windows were checked by rendering the Mac window offscreen and running the sheet in
the iPad simulator.
