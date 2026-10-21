/* GameSettings without a window: default keys, a saved rebinding, the newer of this device's copy and iCloud's, and a
   game's graphics choice applied. Run by run.sh with GAMESETTINGS_LOCAL_DIR and GAMESETTINGS_CLOUD_DIR set. */
#include <stdio.h>
#include <string.h>
#include "../gamesettings.h"

static int failures, applied = -1;
static void expect(const char *name, long long got, long long want) {
    if (got != want) { printf("FAIL %s: got %lld, want %lld\n", name, got, want); failures++; } else printf("ok   %s\n", name);
}
static int get(void) { return 0; }
static void set(int v) { applied = v; }
static const char *const choices[] = {"off", "on", "max"};
static const gamesettings_choice g[] = {{"mode", "Mode", choices, 3, get, set, NULL}};
static const gamesettings_config c = {"0100000000000000", "Test", "[test]", g, 1};

int main(int argc, char **argv) {
    unsigned char held[128] = {0}; int32_t lx, ly, rx, ry;
    const char *phase = argc > 1 ? argv[1] : "defaults";
    if (!strcmp(phase, "defaults")) {   /* nothing started: the default layout */
        held[38] = 1; expect("J is A by default", (long long)gamesettings_buttons(held), 1);
        held[38] = 0; held[13] = 1; gamesettings_sticks(held, &lx, &ly, &rx, &ry); expect("W is left stick up", ly, 32767);
        held[13] = 0; held[49] = 1; expect("Space is B", (long long)gamesettings_buttons(held), 2);
    } else if (!strcmp(phase, "rebound")) {   /* run.sh wrote a keyboard.json binding A to L (37) */
        gamesettings_start(&c);
        held[37] = 1; expect("a saved binding: L is A", (long long)gamesettings_buttons(held), 1);
        held[37] = 0; held[38] = 1; expect("...and J is nothing now", (long long)gamesettings_buttons(held), 0);
        gamesettings_apply_graphics(); expect("the saved graphics choice is applied", applied, 2);
    }
    return failures != 0;
}
