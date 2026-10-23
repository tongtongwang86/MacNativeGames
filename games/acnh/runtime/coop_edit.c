/* Island edits that only the host's copy may carry out (docs/LAN-COOP-DESIGN.md 8.12). Each copy holds the island in
   memory, but only the host saves it: an edit that matters after the session has to happen in the host's copy.

   Museum donations (ACNH_MP_EDIT group "museum"). The museum record keeps each donor as a resident slot of the island,
   so a visitor's donation records nothing (main+0x24819f0 answers -1). acnh_patches.c's stub on the donation's record
   call (main+0xafb53c) credits it to the host's slot instead, records it in the visitor's copy, and puts the item in a
   ring on main's data page. Here, on the game thread:
     visitor  keeps the credit slot at the host's resident slot + 1 (0 outside a session: the game's own behaviour),
              and sends each new ring entry to the host (coop_lan.c EDIT)
     host     records each received item with the same calls the donation action makes: the change notice the save
              keeps (+0x20 of *(island + 0x1bf1b8)), main+0x26bd5b0 "may this be donated", main+0x26bdd90 record.
   The island data is the action's: root = **(main+0x4ab3640); save = *(root+0x10); bit 0 of root+0x453 picks
   save+0x130's object (that result as is) or *save's (result + 0x70), result = that object's virtual +0x38. The museum
   record is *(island+0x1bf1a0) + *(island+0x1bf198), 0x3404 bytes: dates at +4, 0x400 items of 8 bytes at +0x1004,
   0x400 donor slots at +0x3004. */
#include "runtime.h"
#include "coop.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SAVE_ROOT_SLOT 0x4ab3640
#define MUSEUM_MAY_DONATE 0x26bd5b0
#define MUSEUM_RECORD 0x26bdd90
#define DONOR_SLOT 0x24819f0
#define MUSEUM_SIZE 0x3404
enum { EDIT_MUSEUM = 1 };
typedef struct __attribute__((packed)) { uint8_t kind, pad[7]; uint64_t item; } EditBody;
_Static_assert(sizeof(EditBody) == 16, "edit body");

static volatile uint32_t *slots;   /* [0] credit (host's slot + 1), [1] ring head */
static volatile uint64_t *ring;
static uint32_t ring_tail;
void coop_edit_set_queue(volatile uint32_t *s, volatile uint64_t *q) { slots = s; ring = q; }

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static EditBody pending[32]; static int npending;
void coop_edit_received(int node, const void *body, uint32_t n) {
    if (n != sizeof(EditBody)) { fprintf(stderr, "[coop] edit from node %d: %u bytes, ignored\n", node, n); return; }
    pthread_mutex_lock(&lock);
    if (npending < (int)(sizeof pending / sizeof *pending)) memcpy(&pending[npending++], body, sizeof(EditBody));
    else fprintf(stderr, "[coop] edit from node %d dropped: queue full\n", node);
    pthread_mutex_unlock(&lock);
}

/* main+0x24819f0(&player): the island's resident slot of session player <player>, -1 when not a resident */
static int donor_slot(uint8_t *main, int player) {
    uint64_t p = (uint8_t)player;
    return ((int (*)(void *))(uintptr_t)(main + DONOR_SLOT))(&p);
}
static uint8_t *island_data(uint8_t *main) {
    uint64_t slot; memcpy(&slot, main + SAVE_ROOT_SLOT, 8);
    uint8_t *root = slot ? *(uint8_t **)(uintptr_t)slot : NULL;
    if (!root) return NULL;
    uint8_t *save = *(uint8_t **)(root + 0x10), *obj;
    int alt = root[0x453] & 1;
    if (alt) { if (!save || !(obj = *(uint8_t **)(save + 0x130))) return NULL; }
    else { if (!save || !(obj = *(uint8_t **)save)) return NULL; }
    uint8_t *r = ((uint8_t *(*)(void *))(*(void ***)obj)[0x38 / 8])(obj);
    return r && !alt ? r + 0x70 : r;
}
static void notify_change(uint8_t *island) {   /* as the action does around each donation */
    uint8_t *t = *(uint8_t **)(island + 0x1bf1b8); void *obj = t ? *(void **)(t + 0x20) : NULL;
    if (!obj) return;
    struct { uint64_t offset; void *base; uint64_t size; } change = {*(uint64_t *)(island + 0x1bf198), *(void **)(island + 0x1bf1a0), MUSEUM_SIZE};
    ((void (*)(void *, void *))(*(void ***)obj)[0x20 / 8])(obj, &change);
}
static void host_donate(uint8_t *main, uint64_t item) {
    uint8_t *island = island_data(main);
    if (!island || !*(uint8_t **)(island + 0x1bf1a0)) { fprintf(stderr, "[coop] museum: no island data, donation %#llx lost\n", (unsigned long long)item); return; }
    uint8_t *museum = *(uint8_t **)(island + 0x1bf1a0) + *(uint64_t *)(island + 0x1bf198);
    notify_change(island);
    if (!(((uint32_t (*)(void *, void *))(uintptr_t)(main + MUSEUM_MAY_DONATE))(museum, &item) & 1)) {
        fprintf(stderr, "[coop] museum: item %#llx is already in the host's museum (or not a museum item)\n", (unsigned long long)item);
        return;
    }
    int donor = donor_slot(main, 0);
    if (donor < 0) donor = 0;
    notify_change(island);
    ((void (*)(void *, void *, int))(uintptr_t)(main + MUSEUM_RECORD))(museum, &item, donor);
    fprintf(stderr, "[coop] museum: recorded a visitor's donation, item %#llx, credited to resident slot %d\n", (unsigned long long)item, donor);
}

void coop_edit_tick(void) {
    if (!slots) return;
    uint8_t *main = runtime_module_base("main");
    int self = coop_lan_self_node();
    if (self <= 0) {
        slots[0] = 0; ring_tail = slots[1];
        if (self == 0) {
            EditBody e[32]; int n;
            pthread_mutex_lock(&lock); n = npending; memcpy(e, pending, sizeof(EditBody) * (size_t)n); npending = 0; pthread_mutex_unlock(&lock);
            for (int i = 0; i < n; i++) if (e[i].kind == EDIT_MUSEUM) host_donate(main, e[i].item);
        }
        return;
    }
    static unsigned frames; static int last_credit = -2;
    if (frames++ % 30 == 0) {   /* the host's resident slot, as this copy's island data has it */
        int c = donor_slot(main, 0);
        if (c < 0) c = 0;   /* not found: the island's first resident */
        if (c != last_credit) { fprintf(stderr, "[coop] museum: visitor donations credited to resident slot %d\n", c); last_credit = c; }
        slots[0] = (uint32_t)c + 1;
    }
    uint32_t head = slots[1];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    while (ring_tail != head) {
        EditBody e = {.kind = EDIT_MUSEUM, .item = ring[ring_tail & 15]};
        ring_tail++;
        if (coop_lan_send_edit(&e, sizeof e)) fprintf(stderr, "[coop] museum: could not send donation %#llx to the host\n", (unsigned long long)e.item);
        else fprintf(stderr, "[coop] museum: sent donation %#llx to the host\n", (unsigned long long)e.item);
    }
}

/* Pushed furniture on the other copies (acnh_patches.c push-anim): main+0x13773a4 starts an item's move with the
   pushing player taken from the action data, which only the copy that started the push fills; a missing one (> 7)
   becomes the player now in a pull / push / turn state, as main+0x244c554 sees it, so the item follows that player's
   movement as it does on the pusher's own copy. */
#define PLAYER_PUSHING 0x244c554
void coop_fill_pusher(uint8_t *player) {
    if (*player < 8) return;
    uint8_t *main = runtime_module_base("main");
    for (unsigned i = 0; i < 8; i++) {
        uint64_t p = i;
        if (((uint32_t (*)(void *))(uintptr_t)(main + PLAYER_PUSHING))(&p) & 1) { *player = (uint8_t)i; return; }
    }
}
