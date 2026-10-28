/* One "device" for run.sh: device <state dir> <save dir> <version> [commit|busy [command]]: the first sync, then with
   "commit" a save committed during play and the upload it schedules (flushed). command runs between the two, as
   play does (it writes the save; it can also let another device upload meanwhile). With "busy" the upload is not
   flushed: savesync_busy must say 1 at once and 0 once the delayed upload has run (what a quit through _exit waits
   for). Like a game, storage/ is a cache, not a save (savesync_config.exclude; not with DEVICE_OLD set). The cloud
   folder is SAVESYNC_DIR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../savesync.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: device <state dir> <save dir> <version> [commit|busy [command]]\n"); return 2; }
    static const char *const cache[] = {"storage/", NULL};
    savesync_config c = {.title_id = "0100000000000000", .game_version = argv[3], .save_dir = argv[2], .state_dir = argv[1], .log_tag = "[test]",
                         .exclude = getenv("DEVICE_OLD") ? NULL : cache};   /* DEVICE_OLD: a build from before the exclusion */
    if (!savesync_start(&c)) { fprintf(stderr, "[test] sync is off\n"); return 1; }
    if (argc > 4 && !strcmp(argv[4], "commit")) {
        if (argc > 5 && system(argv[5])) return 1;
        savesync_committed(); savesync_flush(10);
    }
    if (argc > 4 && !strcmp(argv[4], "busy")) {
        if (argc > 5 && system(argv[5])) return 1;
        savesync_committed();
        if (!savesync_busy()) { fprintf(stderr, "[test] not busy right after a commit\n"); return 3; }
        for (int i = 0; i < 200 && savesync_busy(); i++) usleep(50000);
        if (savesync_busy()) { fprintf(stderr, "[test] still busy after 10 s\n"); return 4; }
    }
    return 0;
}
