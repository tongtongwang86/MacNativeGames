/* GameSettings: one Settings window (⌘,) for native game ports: keyboard bindings shared by every game, and each
   game's graphics settings. Both are kept in iCloud next to SaveSync's saves (<container>/Documents/Settings/), so
   every game and every Mac uses the same keys, and a game's graphics follow you between Macs.
   The library knows nothing about any game: the port lists its graphics settings as choices (a getter and a setter
   each) and asks the library which Switch buttons the held keys press. See ARCHITECTURE.md next to this file. */
#pragma once
#include <stdint.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A graphics setting: one of a few choices, read and applied by the game. get/set run on the main thread. */
typedef struct {
    const char *id;                 /* stable name in the settings file, e.g. "picture" */
    const char *title;              /* shown in the window, e.g. "Picture" */
    const char *const *choices;     /* choice titles, index = the value */
    unsigned nchoices;
    int (*get)(void);               /* the choice in force now (also after a shortcut key changed it) */
    void (*set)(int choice);        /* apply a choice now */
    const char *note;               /* optional line under the setting */
} gamesettings_choice;

typedef struct {
    const char *title_id;           /* the game's graphics file is Settings/<title_id>.json */
    const char *game_name;          /* the window title: "<game> Settings" */
    const char *log_tag;            /* log line prefix, e.g. "[totk]" */
    const gamesettings_choice *graphics;
    unsigned ngraphics;
} gamesettings_config;

/* Loads the settings (the newer of this Mac's copy and iCloud's) and makes the keyboard bindings live. Call early,
   on any thread; the config is kept by reference (static storage). */
void gamesettings_start(const gamesettings_config *config);
/* Applies the saved graphics settings; call on the main thread once the renderer can take them. */
void gamesettings_apply_graphics(void);
/* Saves the graphics settings as they are now (a shortcut key changed one). Cheap; any thread. */
void gamesettings_note_graphics_changed(void);

/* Where the game keeps its saves, for the Saves tab's buttons (Show in Finder, Open in Files). cloud: the iCloud copy
   (savesync_cloud_folder()), NULL without one. Any thread; call again if they change. */
void gamesettings_set_save_folders(const char *local, const char *cloud);

/* ---- keyboard (Mac virtual key codes, as NSEvent.keyCode) ---- */
/* Switch buttons pressed by the held keys (libnx HID bits: A 1<<0, B, X, Y, L-stick, R-stick, L, R, ZL, ZR, Plus,
   Minus, D-pad left, up, right, down). */
uint64_t gamesettings_buttons(const unsigned char held[128]);
/* Stick positions from the held keys, -32767..32767 (up and right positive). */
void gamesettings_sticks(const unsigned char held[128], int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
/* An iOS hardware key (UIKey.keyCode, a USB HID usage) as the Mac key code the bindings use, -1 for none. */
int gamesettings_mac_key_from_hid(long usage);

#if defined(__OBJC__) && !TARGET_OS_IPHONE
@class NSMenu;
/* Adds "Settings…  ⌘," to the app menu (before its first item). */
void gamesettings_add_menu_item(NSMenu *app_menu);
#endif
/* Opens the Settings window (Mac) or sheet (iPhone / iPad). Any thread. */
void gamesettings_show(void);

#ifdef __cplusplus
}
#endif
