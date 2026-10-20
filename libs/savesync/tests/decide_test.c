/* savesync_decide against the table in ARCHITECTURE.md. */
#include <stdio.h>
#include "../savesync.h"

static int failures;
static void expect(const char *name, savesync_facts f, savesync_action want, int want_conflict) {
    int conflict; savesync_action got = savesync_decide(&f, &conflict);
    if (got != want || conflict != want_conflict) { printf("FAIL %s: got %d/%d, want %d/%d\n", name, got, conflict, want, want_conflict); failures++; }
    else printf("ok   %s\n", name);
}

int main(void) {
    expect("nothing anywhere", (savesync_facts){.local_empty = 1}, SAVESYNC_NOTHING, 0);
    expect("first device sends", (savesync_facts){0}, SAVESYNC_SEND, 0);
    expect("copy still arriving", (savesync_facts){.cloud_exists = 1}, SAVESYNC_WAIT, 0);
    expect("same saves", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .local_is_cloud = 1}, SAVESYNC_IN_STEP, 0);
    expect("new device takes", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .local_empty = 1}, SAVESYNC_TAKE, 0);
    expect("cloud changed", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .local_is_last = 1}, SAVESYNC_TAKE, 0);
    expect("local changed", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .cloud_is_last = 1}, SAVESYNC_SEND, 0);
    expect("both changed, cloud newer", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .cloud_newer_time = 1}, SAVESYNC_TAKE, 1);
    expect("both changed, local newer", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1}, SAVESYNC_SEND, 1);
    expect("newer game version", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .local_is_last = 1, .cloud_newer_version = 1}, SAVESYNC_REFUSE_NEWER, 0);
    expect("newer version, empty here", (savesync_facts){.cloud_exists = 1, .cloud_complete = 1, .local_empty = 1, .cloud_newer_version = 1}, SAVESYNC_REFUSE_NEWER, 0);
    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures != 0;
}
