/* SaveSync: iCloud save sync for native game ports (Mac and iPhone / iPad).
   One game process keeps one save folder in step with a copy in iCloud. The library knows nothing about any game: the
   port describes its save folder in a savesync_config, calls savesync_start once before the game reads its saves and
   savesync_committed after each save the game commits. See ARCHITECTURE.md next to this file. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title_id;       /* names the game's folder in iCloud (SaveSync/<title_id>), e.g. "0100F2C0115B6000" */
    const char *game_version;   /* written with each upload; a save from a newer version is never taken ("1.4.3") */
    const char *save_dir;       /* the folder whose contents are synced (only the contents: account ids may differ) */
    const char *state_dir;      /* this device's sync state and save-backups/ (a folder the game does not sync) */
    const char *container_id;   /* iCloud container ("iCloud.com.example.game"); NULL: the app's first container on
                                   iOS, any container that already holds SaveSync/<title_id> on a Mac */
    const char *log_tag;        /* log line prefix, e.g. "[totk]"; NULL: "[savesync]" */
    int (*save_busy)(void);     /* the game is writing saves right now (uploads wait); may be NULL. The game's own
                                   writes only: not savesync_busy, or an upload would wait for itself */
    unsigned keep_backups;      /* replaced saves kept in state_dir/save-backups; 0: 10 */
    const char *const *exclude; /* NULL-terminated paths under save_dir that are not saves (a cache the game rewrites at
                                   every start): never synced, hashed or dated, and never removed when the cloud's saves
                                   are taken. A path ending in "/" covers a folder. A cloud copy sent before a path
                                   was excluded is still read; the next send drops the path from it. May be NULL */
} savesync_config;

/* Finds the cloud folder and does the first sync, blocking (seconds at most; iOS waits up to 20 s for iCloud's
   metadata and a minute per missing file). Call it on a thread other than the main thread, before the game reads its
   saves. Returns 1 when syncing is on, 0 when it is off (no iCloud, SAVESYNC=0, no container). The config's strings
   are copied. */
int savesync_start(const savesync_config *config);
/* The game committed a save: upload it a few seconds later, once the game has stopped writing. Cheap; any thread. */
void savesync_committed(void);
/* Upload now if there is something to upload, waiting up to timeout_s seconds (app going to the background, quit). */
void savesync_flush(double timeout_s);
/* An upload is scheduled or running: a quit that skips atexit (_exit) should wait while this is 1. Any thread. */
int savesync_busy(void);
/* The game's folder in iCloud (SaveSync/<title id>), NULL when syncing is off. */
const char *savesync_cloud_folder(void);

/* ---- the decision, exposed for tests and tools ---- */
typedef enum {
    SAVESYNC_NOTHING,       /* nothing here and nothing in the cloud */
    SAVESYNC_IN_STEP,       /* same saves on both sides */
    SAVESYNC_TAKE,          /* replace this device's saves with the cloud's */
    SAVESYNC_SEND,          /* replace the cloud's saves with this device's */
    SAVESYNC_WAIT,          /* the cloud copy is still arriving: keep this device's for now */
    SAVESYNC_REFUSE_NEWER,  /* the cloud save comes from a newer game version than this one */
} savesync_action;
typedef struct {
    int cloud_exists;       /* a manifest is there */
    int cloud_complete;     /* its files are here and hash to it */
    int local_empty;        /* this device has no save files */
    int local_is_cloud;     /* local hash == cloud hash */
    int local_is_last;      /* local hash == the hash both sides last agreed on */
    int cloud_is_last;      /* cloud hash == that hash */
    int cloud_newer_version;/* the cloud's game version is newer than this game's */
    int cloud_newer_time;   /* the cloud copy was written after this device's newest save file changed */
} savesync_facts;
/* *conflict is set when both sides changed since they last agreed (the loser is backed up). */
savesync_action savesync_decide(const savesync_facts *facts, int *conflict);

#ifdef __cplusplus
}
#endif
