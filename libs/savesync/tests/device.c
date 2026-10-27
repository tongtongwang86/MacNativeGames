/* One "device" for run.sh: device <state dir> <save dir> <version> [commit [command]]: the first sync, then with
   "commit" a save committed during play and the upload it schedules (flushed). command runs between the two, as
   play does (it writes the save; it can also let another device upload meanwhile). The cloud folder is SAVESYNC_DIR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../savesync.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: device <state dir> <save dir> <version> [commit]\n"); return 2; }
    savesync_config c = {.title_id = "0100000000000000", .game_version = argv[3], .save_dir = argv[2], .state_dir = argv[1], .log_tag = "[test]"};
    if (!savesync_start(&c)) { fprintf(stderr, "[test] sync is off\n"); return 1; }
    if (argc > 4 && !strcmp(argv[4], "commit")) {
        if (argc > 5 && system(argv[5])) return 1;
        savesync_committed(); savesync_flush(10);
    }
    return 0;
}
