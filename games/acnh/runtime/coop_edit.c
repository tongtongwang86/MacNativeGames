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
#include <stdlib.h>
#include <string.h>

#define SAVE_ROOT_SLOT 0x4ab3640
#define MUSEUM_MAY_DONATE 0x26bd5b0
#define MUSEUM_RECORD 0x26bdd90
#define DONOR_SLOT 0x24819f0
#define MUSEUM_SIZE 0x3404
enum { EDIT_MUSEUM = 1, EDIT_ROOM = 2, EDIT_ORDER = 3, EDIT_POSTBOX = 5, EDIT_LETTERS = 6, EDIT_HOTEL_GO = 7, EDIT_HOTEL_END = 8 };
typedef struct __attribute__((packed)) { uint8_t kind, pad[7]; uint64_t item; } EditBody;
_Static_assert(sizeof(EditBody) == 16, "edit body");
/* One decorating-mode change of a room tile: op 1 clears the tile, 5 puts the record there (the furniture ops' codes) */
typedef struct __attribute__((packed)) { uint8_t kind, op, layer, flag; uint32_t slot, map; int16_t x, z; uint64_t record; } RoomEdit;
_Static_assert(sizeof(RoomEdit) == 24, "room edit");

static volatile uint32_t *slots;   /* [0] credit (host's slot + 1), [1] ring head */
static volatile uint64_t *ring;
static uint32_t ring_tail;
void coop_edit_set_queue(volatile uint32_t *s, volatile uint64_t *q) { slots = s; ring = q; }

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static EditBody pending[32]; static int npending;
static void room_received(int node, const RoomEdit *e);
static void post_received(int node, const uint8_t *body, uint32_t n);
void coop_edit_received(int node, const void *body, uint32_t n) {
    if (n >= 8 && (*(const uint8_t *)body == EDIT_POSTBOX || *(const uint8_t *)body == EDIT_LETTERS)) { post_received(node, body, n); return; }
    if (n == sizeof(RoomEdit) && *(const uint8_t *)body == EDIT_ROOM) { room_received(node, body); return; }
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

/* Nook Shopping orders (acnh_patches.c group "mail"; LAN-COOP-DESIGN.md 8.20). Placing an order charges the player's
   wallet (main+0x2579c50), then main+0x1a49ab0(&item, &date, &player) puts it in one of 5 order slots of that player's
   personal data (base+0xa900 + i*0x2ec), delivered as a letter the next day. A visitor has no mailbox on the island, so
   her order is recorded by the host's copy as the host's own: it arrives in the host's mailbox. Her copy still charges
   her wallet; the record call is replaced (coop_order_record) and sends the item (EDIT kind 3). The host records it with
   today's date ({u16 year, u8 month, u8 day} at **(main+0x4ab2278) + 0x4060, as the order action reads it). */
#define ORDER_RECORD 0x1a49ab0
#define CLOCK_SLOT 0x4ab2278
typedef int (*order_fn)(const void *, const void *, const void *);
int coop_order_record(const uint64_t *item, const uint32_t *date, const uint8_t *player) {
    uint8_t *main = runtime_module_base("main");
    if (coop_lan_self_node() <= 0) return ((order_fn)(uintptr_t)(main + ORDER_RECORD))(item, date, player);
    EditBody e = {.kind = EDIT_ORDER, .item = *item};
    if (coop_lan_send_edit(&e, sizeof e)) { fprintf(stderr, "[coop] order: could not send %#llx to the host\n", (unsigned long long)*item); return 0; }
    fprintf(stderr, "[coop] order: sent %#llx to the host's mailbox\n", (unsigned long long)*item);
    return 1;
}
static void host_order(uint8_t *main, uint64_t item) {
    uint8_t *clock = **(uint8_t ***)(void *)(main + CLOCK_SLOT);
    uint32_t date = 0; if (clock) memcpy(&date, clock + 0x4060, 4);
    uint8_t host = 0;
    int ok = ((order_fn)(uintptr_t)(main + ORDER_RECORD))(&item, &date, &host);
    fprintf(stderr, ok ? "[coop] order: a visitor's order %#llx is in the host's mail for tomorrow\n"
                       : "[coop] order: the host's 5 order slots are full, a visitor's order %#llx is lost\n", (unsigned long long)item);
}

/* The host's mailbox for a visitor (group "mail"; LAN-COOP-DESIGN.md 8.21). Mail is a save file of its own per player
   (postbox.dat, 0x47430 bytes: 300 letters of 0x3c8 bytes at +0x110, u32 display order[300] at +0x46f70, a free
   index at +0x4741c), and the mailbox screen shows the postbox main+0x25f2a00(save) picks: the local player's. So:
     host     sends its postbox to the visitors whenever it changes (EDIT kind 5), and writes the letters a visitor
              changed (kind 6) into its own, with the save's change notice (*(*(h+0x28)+0x20), vtable +0x20) and the
              game's recount main+0x2726200(data, &player)
     visitor  keeps that copy; while it has one, main+0x25f2a00 returns a handle on it (coop_postbox), so the mailbox
              screen and the letter actions (read, take the item into her own pockets, delete) work on the host's
              letters; every half second the letters that differ from what the host sent go back to the host.
   A handle h: data = *(h+0x10) + *(h+8); +0x28 the change notice (0: none). */
#define POSTBOX_SIZE 0x47430
#define POST_LETTERS 0x110
#define POST_LETTER 0x3c8
#define POST_ORDER 0x46f70
#define POST_FREE 0x4741c
#define POST_RECOUNT 0x2726200
extern uint64_t coop_postbox_original;
typedef uint64_t (*postbox_fn)(void *);
static uint8_t *post_shadow, *post_base;   /* visitor: the host's postbox as edited here / as last sent by the host */
static int post_valid;
static uint64_t post_handle[8];
static uint8_t *post_in; static uint32_t post_in_n;   /* visitor: the latest kind 5 from the network thread */
static uint8_t *letters_in[16]; static uint32_t letters_in_n[16]; static int nletters_in;   /* host: kind 6 */

static uint8_t *save_root(uint8_t *main) {
    uint64_t slot; memcpy(&slot, main + SAVE_ROOT_SLOT, 8);
    return slot ? *(uint8_t **)(uintptr_t)slot : NULL;
}
static uint8_t *postbox_data(uint64_t h) { return h ? *(uint8_t **)(uintptr_t)(h + 0x10) + *(uint64_t *)(uintptr_t)(h + 8) : NULL; }
static void post_visitor_send(void);
static int post_open;   /* visitor: from the mailbox screen's start to the end of the mailbox action */
void coop_post_open(void) {
    if (coop_lan_self_node() <= 0 || !post_valid) return;
    post_open = 1;
    fprintf(stderr, "[coop] mail: the mailbox screen shows the host's letters\n");
}
void coop_post_close(void) {
    if (!post_open) return;
    post_open = 0;
    if (post_valid) post_visitor_send();
    fprintf(stderr, "[coop] mail: mailbox closed, her own mail again\n");
}
uint64_t coop_postbox(void *save) {
    uint64_t r = coop_postbox_original ? ((postbox_fn)(uintptr_t)coop_postbox_original)(save) : 0;
    if (!post_valid || !post_open) return r;
    post_handle[1] = 0; post_handle[2] = (uint64_t)(uintptr_t)post_shadow; post_handle[5] = 0;
    return (uint64_t)(uintptr_t)post_handle;
}
static void post_received(int node, const uint8_t *body, uint32_t n) {   /* network thread */
    uint8_t *copy = malloc(n); if (!copy) return;
    memcpy(copy, body, n);
    pthread_mutex_lock(&lock);
    if (body[0] == EDIT_POSTBOX) { if (n == 8 + POSTBOX_SIZE) { free(post_in); post_in = copy; post_in_n = n; copy = NULL; } }
    else if (nletters_in < 16) { letters_in[nletters_in] = copy; letters_in_n[nletters_in++] = n; copy = NULL; }
    pthread_mutex_unlock(&lock);
    if (copy) { fprintf(stderr, "[coop] mail: message kind %u from node %d (%u bytes) dropped\n", body[0], node, n); free(copy); }
}
static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; while (n--) h = (h ^ *p++) * 1099511628211ull; return h; }
static void host_post_notify(uint64_t h) {
    uint8_t *n = *(uint8_t **)(uintptr_t)(h + 0x28); void *obj = n ? *(void **)(n + 0x20) : NULL;
    if (!obj) return;
    struct { uint64_t off; void *base; uint64_t size; } change = {*(uint64_t *)(uintptr_t)(h + 8), *(void **)(uintptr_t)(h + 0x10), POSTBOX_SIZE};
    ((void (*)(void *, void *))(*(void ***)obj)[0x20 / 8])(obj, &change);
}
static void post_host_tick(uint8_t *main) {
    uint8_t *root = save_root(main);
    uint64_t h = root && coop_postbox_original ? ((postbox_fn)(uintptr_t)coop_postbox_original)(root) : 0;
    uint8_t *data = postbox_data(h);
    uint8_t *in[16]; uint32_t inn[16]; int n;
    pthread_mutex_lock(&lock); n = nletters_in; memcpy(in, letters_in, sizeof *in * (size_t)n); memcpy(inn, letters_in_n, sizeof *inn * (size_t)n); nletters_in = 0; pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {   /* {u8 6, u8 has order, u16 count, u32 free index} [order u32[300]] count x {u16 index, u16 0, letter} */
        const uint8_t *b = in[i]; uint32_t len = inn[i], at = 8; uint16_t count; memcpy(&count, b + 2, 2);
        if (data) {
            host_post_notify(h);
            if (b[1] && at + 300 * 4 <= len) { memcpy(data + POST_ORDER, b + at, 300 * 4); memcpy(data + POST_FREE, b + 4, 4); at += 300 * 4; }
            for (unsigned k = 0; k < count && at + 4 + POST_LETTER <= len; k++, at += 4 + POST_LETTER) {
                uint16_t idx; memcpy(&idx, b + at, 2);
                if (idx < 300) memcpy(data + POST_LETTERS + (size_t)idx * POST_LETTER, b + at + 4, POST_LETTER);
            }
            uint8_t host = 0;
            ((void (*)(void *, void *))(uintptr_t)(main + POST_RECOUNT))(data, &host);
            fprintf(stderr, "[coop] mail: a visitor changed %u letter(s) in the host's mailbox%s\n", count, b[1] ? " (and their order)" : "");
        }
        free(in[i]);
    }
    static unsigned frames; static uint64_t last; static int last_visitors;
    if (frames++ % 30 || !data) return;
    int visitors = coop_lan_host_visitors(0);
    if (!visitors) { last_visitors = 0; return; }
    uint64_t sum = fnv(data, POSTBOX_SIZE);
    if (sum == last && visitors == last_visitors) return;
    static uint8_t msg[8 + POSTBOX_SIZE];
    msg[0] = EDIT_POSTBOX; memcpy(msg + 8, data, POSTBOX_SIZE);
    coop_lan_broadcast_edit(msg, sizeof msg, -1);
    last = sum; last_visitors = visitors;
}
static void post_visitor_send(void) {   /* the letters this copy changed since the host's last copy */
    static uint8_t msg[8 + 300 * 4 + 300 * (4 + POST_LETTER)];
    uint32_t at = 8; uint16_t count = 0;
    int order = memcmp(post_shadow + POST_ORDER, post_base + POST_ORDER, 300 * 4) || memcmp(post_shadow + POST_FREE, post_base + POST_FREE, 4);
    if (order) { memcpy(msg + at, post_shadow + POST_ORDER, 300 * 4); at += 300 * 4; }
    for (uint16_t i = 0; i < 300; i++) {
        const uint8_t *a = post_shadow + POST_LETTERS + (size_t)i * POST_LETTER, *b = post_base + POST_LETTERS + (size_t)i * POST_LETTER;
        if (!memcmp(a, b, POST_LETTER)) continue;
        memcpy(msg + at, &i, 2); memset(msg + at + 2, 0, 2); memcpy(msg + at + 4, a, POST_LETTER); at += 4 + POST_LETTER; count++;
    }
    if (!count && !order) return;
    msg[0] = EDIT_LETTERS; msg[1] = (uint8_t)order; memcpy(msg + 2, &count, 2); memcpy(msg + 4, post_shadow + POST_FREE, 4);
    if (coop_lan_send_edit(msg, at)) { fprintf(stderr, "[coop] mail: could not send %u changed letter(s) to the host\n", count); return; }
    memcpy(post_base, post_shadow, POSTBOX_SIZE);
    fprintf(stderr, "[coop] mail: sent %u changed letter(s) to the host\n", count);
}
static void post_visitor_tick(int self) {
    if (self <= 0) { post_valid = 0; post_open = 0; return; }
    static unsigned frames;
    if (post_valid && frames++ % 15 == 0) post_visitor_send();
    uint8_t *in = NULL;
    pthread_mutex_lock(&lock); in = post_in; post_in = NULL; pthread_mutex_unlock(&lock);
    if (!in) return;
    if (!post_shadow) { post_shadow = malloc(POSTBOX_SIZE); post_base = malloc(POSTBOX_SIZE); }
    if (post_shadow && post_base) {
        if (post_valid) post_visitor_send();
        memcpy(post_shadow, in + 8, POSTBOX_SIZE); memcpy(post_base, in + 8, POSTBOX_SIZE);
        if (!post_valid) fprintf(stderr, "[coop] mail: have the host's mailbox\n");
        post_valid = 1;
    }
    free(in);
}

/* A visitor follows the host's decorating job (group "hotel"; LAN-COOP-DESIGN.md 8.22). Each copy has one hotel
   manager, *(main+0x5139df0): +8 VIP flag, +0xc room index; main+0x15bf0b0(manager) copies that room of the island into
   the player's own working area before the stage change "cHotelSelectRoom" opens IdrHotel / Room<n>. The host's copy
   sends the room (EDIT kind 7) and the job's end (kind 8); the visitor's copy sets the same room, prepares it and runs
   the overlay's Coop_LocalHotel stage changes (into the room / back to the lobby). */
#define HOTEL_MANAGER 0x5139df0
#define HOTEL_PREPARE 0x15bf0b0
static int hotel_following;
void coop_hotel_go(uint8_t *manager) {
    if (hotel_following || coop_lan_self_node() != 0 || !manager || !coop_lan_host_visitors(0)) return;
    EditBody e = {.kind = EDIT_HOTEL_GO, .item = (uint64_t)manager[8] | (uint64_t)*(uint32_t *)(manager + 0xc) << 32};
    coop_lan_broadcast_edit(&e, sizeof e, -1);
    fprintf(stderr, "[coop] hotel: the host goes to %s %d, visitors follow\n", manager[8] ? "VIP room" : "room", *(int32_t *)(manager + 0xc));
}
void coop_hotel_end(uint8_t *manager) {
    (void)manager;
    if (coop_lan_self_node() != 0 || !coop_lan_host_visitors(0)) return;
    EditBody e = {.kind = EDIT_HOTEL_END};
    coop_lan_broadcast_edit(&e, sizeof e, -1);
    fprintf(stderr, "[coop] hotel: the host's job ends, visitors go back to the lobby\n");
}
static void hotel_visitor(uint8_t *main, const EditBody *e) {
    uint8_t *manager = *(uint8_t **)(void *)(main + HOTEL_MANAGER);
    if (e->kind == EDIT_HOTEL_GO) {
        if (!manager) { fprintf(stderr, "[coop] hotel: no hotel manager on this copy, cannot follow\n"); return; }
        manager[8] = (uint8_t)e->item; *(int32_t *)(manager + 0xc) = (int32_t)(e->item >> 32);
        hotel_following = 1;
        ((void (*)(void *))(uintptr_t)(main + HOTEL_PREPARE))(manager);
        hotel_following = 0;
        coop_flow_test("Coop_LocalHotel:Coop_HotelRoom");
        fprintf(stderr, "[coop] hotel: following the host to %s %d\n", manager[8] ? "VIP room" : "room", *(int32_t *)(manager + 0xc));
    } else {
        coop_flow_test("Coop_LocalHotel:Coop_HotelBack");
        fprintf(stderr, "[coop] hotel: back to the lobby with the host\n");
    }
}

static void room_tick(uint8_t *main, int self);
void coop_edit_tick(void) {
    uint8_t *main = runtime_module_base("main");
    int self = coop_lan_self_node();
    room_tick(main, self);
    if (self == 0) post_host_tick(main); else post_visitor_tick(self);
    if (self <= 0) {
        if (slots) { slots[0] = 0; ring_tail = slots[1]; }
        if (self == 0) {
            EditBody e[32]; int n;
            pthread_mutex_lock(&lock); n = npending; memcpy(e, pending, sizeof(EditBody) * (size_t)n); npending = 0; pthread_mutex_unlock(&lock);
            for (int i = 0; i < n; i++) {
                if (e[i].kind == EDIT_MUSEUM) host_donate(main, e[i].item);
                else if (e[i].kind == EDIT_ORDER) host_order(main, e[i].item);
            }
        }
        return;
    }
    {   /* visitor: what the host sends (the hotel job) */
        EditBody e[32]; int n;
        pthread_mutex_lock(&lock); n = npending; memcpy(e, pending, sizeof(EditBody) * (size_t)n); npending = 0; pthread_mutex_unlock(&lock);
        for (int i = 0; i < n; i++) if (e[i].kind == EDIT_HOTEL_GO || e[i].kind == EDIT_HOTEL_END) hotel_visitor(main, &e[i]);
    }
    if (!slots) return;   /* museum: the visitor's side below */
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

/* Decorating mode in company (acnh_patches.c group "decor-sync"; LAN-COOP-DESIGN.md 8.18). The decorating editor moves
   the room's furniture actors itself, and each actor writes its tile into the room's item layer
   (main+0x137ec00 -> main+0x137d500 -> ... -> main+0x1950560 put a record / main+0x19508a0 clear a tile, both
   (ctx, layer, ...) with ctx {vtable, u32 map slot @8 (0 = the current map), u32 map id @0xc, u8 @0x10} and a tile
   {vtable, s32 x @8, s32 z @0xc}). Nothing of it reaches the other copies, which replay only the ordinary furniture
   actions (the furniture ops of main+0x21cfab0, run by every copy). So the stubs on those two layer writes queue the
   writes made from the editor's code (a return address in main+0x13c0000..0x13f8000 on the call chain) and this file
   sends them; a copy that receives one carries it out with the ordinary furniture ops' own handlers, which create or
   remove the actor and write the layer: op 1 (main+0x21d0acc, packet {u8 1, s16 x @2, s16 z @4, u8 layer @6}) and op 5
   (main+0x21dc0e8, clears the tile, then puts the record: {u8 5, record @2, s16 x @0xa, s16 z @0xc, u8 layer @0xe,
   options @0xf..0x13, player @0x68 = 0xff}). Both read only the packet at job+0x28, so a zeroed handler and job do. */
#define EDITOR_LO 0x13c0000
#define EDITOR_HI 0x13f8000
#define OP_REMOVE 0x21d0acc
#define OP_REPLACE 0x21dc0e8
#define OP_PLACE 0x21da650   /* op 0, placing from pockets: the same packet as op 5, without the clear */
static int room_session;   /* in a session: the stubs do any work at all */
static RoomEdit room_out[64]; static int nroom_out;
static RoomEdit room_in[64]; static int nroom_in;
static int applying;

static int from_editor(uint64_t lr) {
    uint8_t *main = runtime_module_base("main");
    uint64_t lo = (uint64_t)(uintptr_t)main + EDITOR_LO, hi = (uint64_t)(uintptr_t)main + EDITOR_HI;
    if (lr >= lo && lr < hi) return 1;
    uint64_t *fp = __builtin_frame_address(0);
    for (int i = 0; i < 24 && fp; i++) {
        uint64_t *next = (uint64_t *)(uintptr_t)fp[0], ret = fp[1];
        if (ret >= lo && ret < hi) return 1;
        if (next <= fp || (uint8_t *)next - (uint8_t *)fp > 0x100000) break;
        fp = next;
    }
    return 0;
}
static void room_queue(uint8_t *ctx, uint8_t op, uint32_t layer, const uint8_t *pos, uint64_t record, uint64_t lr) {
    if (!room_session || applying || !ctx || !pos || !from_editor(lr)) return;
    RoomEdit e = {.kind = EDIT_ROOM, .op = op, .layer = (uint8_t)layer, .flag = ctx[0x10], .slot = *(uint32_t *)(ctx + 8),
                  .map = *(uint32_t *)(ctx + 0xc), .x = (int16_t)*(int32_t *)(pos + 8), .z = (int16_t)*(int32_t *)(pos + 0xc), .record = record};
    fprintf(stderr, "[coop] room: decorating %s slot %u map %#x layer %u (%d,%d) %016llx\n", op == 5 ? "put" : "clear",
            e.slot, e.map, e.layer, e.x, e.z, (unsigned long long)record);
    if (nroom_out < 64) room_out[nroom_out++] = e;
}
void coop_room_set(uint8_t *ctx, uint32_t layer, const uint64_t *record, const uint8_t *pos, uint64_t lr) {
    if (record) room_queue(ctx, 5, layer, pos, *record, lr);
}
void coop_room_clear(uint8_t *ctx, uint32_t layer, const uint8_t *pos, uint64_t unused, uint64_t lr) {
    (void)unused; room_queue(ctx, 1, layer, pos, 0, lr);
}
void coop_room_op_seen(void *handler, uint8_t *job, uint64_t a2, uint64_t a3, uint64_t lr) {
    (void)handler; (void)a2; (void)a3; (void)lr;
    uint8_t *p = room_session && job ? *(uint8_t **)(job + 0x28) : NULL;
    if (!p) return;
    fprintf(stderr, "[coop] room: furniture op %u: record %016llx (%d,%d) layer %u options %02x %02x %02x %02x %02x player %u\n",
            p[0], (unsigned long long)*(uint64_t *)(void *)(p + 2), *(int16_t *)(void *)(p + 0xa), *(int16_t *)(void *)(p + 0xc),
            p[0xe], p[0xf], p[0x10], p[0x11], p[0x12], p[0x13], p[0x68]);
}

static int room_in_node[64]; static uint8_t room_in_opts[64][6];   /* [0] set: options @0xf..0x13 from [1..5] (diagnostics) */
static void room_received(int node, const RoomEdit *e) {   /* network thread */
    pthread_mutex_lock(&lock);
    if (nroom_in < 64) { room_in[nroom_in] = *e; room_in_node[nroom_in] = node; room_in_opts[nroom_in][0] = 0; nroom_in++; }
    else fprintf(stderr, "[coop] room: edit from node %d dropped: queue full\n", node);
    pthread_mutex_unlock(&lock);
}

/* The map the furniture ops act on, as main+0x21d0acc finds it: the scene object *(main+0x4ab16b0) names a scene
   (vtable +0x18 of *(main+0x4ab15d0), hashed by main+0x5bd550); scene 0x758f7eaf takes its map from *(main+0x4ac2a10),
   any other from main+0x54bf480; the map id is that object's +0x38. */
static uint32_t current_map(uint8_t *main) {
    uint8_t *scene = **(uint8_t ***)(void *)(main + 0x4ab16b0), *cur = main + 0x54bf480;
    if (scene && *(uint64_t *)(void *)(scene + 0x330)) {
        void *names = **(void ***)(void *)(main + 0x4ab15d0);
        uint64_t *name = ((uint64_t *(*)(void *, uint32_t))(*(void ***)names)[0x18 / 8])(names, *(uint32_t *)(void *)(scene + 0x338));
        if (name && ((int (*)(uint64_t, int))(uintptr_t)(main + 0x5bd550))(*name, 0) == 0x758f7eaf) cur = *(uint8_t **)(void *)(main + 0x4ac2a10);
    }
    return cur ? *(uint32_t *)(void *)(cur + 0x38) : 0xffffffffu;
}
static void room_apply(uint8_t *main, const RoomEdit *e, const uint8_t *opts) {
    static uint8_t handler[0x378], job[0x40], packet[0x70];
    uint32_t map = current_map(main);
    if (e->slot != 0 || (e->map != map && e->map != 0xffffffffu)) {   /* 0xffffffff: coop room, the current map */
        fprintf(stderr, "[coop] room: %s (%d,%d) for map %#x slot %u not applied: this copy is in map %#x\n", e->op == 5 ? "put" : "clear", e->x, e->z, e->map, e->slot, map);
        return;
    }
    memset(handler, 0, sizeof handler); memset(job, 0, sizeof job); memset(packet, 0, sizeof packet);
    *(uint8_t **)(void *)(job + 0x28) = packet;
    int wall = e->op == 5 && e->layer >= 2;
    packet[0] = wall ? 0 : e->op; packet[0x68] = 0xff;
    if (e->op == 5) {
        memcpy(packet + 2, &e->record, 8); memcpy(packet + 0xa, &e->x, 2); memcpy(packet + 0xc, &e->z, 2); packet[0xe] = e->layer;
        /* wall items: hanging items placed from pockets came as layer 2 with options 00 00 01 (back wall) and layer 3
           with 01 00 01 (side wall); options all 0 put them on the floor */
        if (e->layer >= 2) { packet[0xf] = (uint8_t)(e->layer - 2); packet[0x11] = 1; }
        if (opts && opts[0]) memcpy(packet + 0xf, opts + 1, 5);
    } else {
        memcpy(packet + 2, &e->x, 2); memcpy(packet + 4, &e->z, 2); packet[6] = e->layer;
    }
    applying = 1;
    /* a wall item put by op 5 faced the room as on the back wall even with the options and record a placement from
       pockets has (op 0 hangs it right), so wall items go through op 0 */
    ((void (*)(void *, void *))(uintptr_t)(main + (wall ? OP_PLACE : e->op == 5 ? OP_REPLACE : OP_REMOVE)))(handler, job);
    applying = 0;
    fprintf(stderr, "[coop] room: applied %s (%d,%d) layer %u %016llx\n", e->op == 5 ? "put" : "clear", e->x, e->z, e->layer, (unsigned long long)e->record);
}
static void room_tick(uint8_t *main, int self) {
    room_session = self >= 0;
    if (!room_session) { nroom_out = 0; return; }
    /* this copy's decorating: drop writes that a later one in the same batch replaces, then send */
    for (int i = 0; i < nroom_out; i++) {
        RoomEdit *e = &room_out[i]; int later = 0;
        for (int j = i + 1; j < nroom_out && !later; j++)
            later = room_out[j].slot == e->slot && room_out[j].map == e->map && room_out[j].layer == e->layer && room_out[j].x == e->x && room_out[j].z == e->z;
        if (later) continue;
        if (self == 0) coop_lan_broadcast_edit(e, sizeof *e, -1);
        else if (coop_lan_send_edit(e, sizeof *e)) fprintf(stderr, "[coop] room: could not send an edit to the host\n");
    }
    nroom_out = 0;
    RoomEdit in[64]; int from[64], n; uint8_t opts[64][6];
    pthread_mutex_lock(&lock); n = nroom_in; memcpy(in, room_in, sizeof *in * (size_t)n); memcpy(from, room_in_node, sizeof *from * (size_t)n); memcpy(opts, room_in_opts, sizeof *opts * (size_t)n); nroom_in = 0; pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {
        if (self == 0 && from[i] >= 0) coop_lan_broadcast_edit(&in[i], sizeof in[i], from[i]);   /* the host passes a visitor's edit on */
        room_apply(main, &in[i], opts[i]);
    }
}

/* "coop room <op> <layer> <x> <z> <hex record> [<hex options @0xf..0x13>]" (input.c, diagnostics): run furniture op
   1 or 5 on this copy as a received edit would, in the current map, e.g. to try the options of a wall item */
void coop_room_debug(const char *args) {
    unsigned op, layer; int x, z; unsigned long long record = 0, o = 0; char ob[16] = "";
    int n = sscanf(args, "%u %u %d %d %llx %15s", &op, &layer, &x, &z, &record, ob);
    if (n < 4 || (op != 1 && op != 5)) { fprintf(stderr, "[coop] room: coop room <1|5> <layer> <x> <z> <record> [<options>]\n"); return; }
    RoomEdit e = {.kind = EDIT_ROOM, .op = (uint8_t)op, .layer = (uint8_t)layer, .slot = 0, .map = 0xffffffffu, .x = (int16_t)x, .z = (int16_t)z, .record = record};
    pthread_mutex_lock(&lock);
    if (nroom_in < 64) {
        room_in[nroom_in] = e; room_in_node[nroom_in] = -2; room_in_opts[nroom_in][0] = n == 6;
        if (n == 6) { o = strtoull(ob, NULL, 16); for (int i = 0; i < 5; i++) room_in_opts[nroom_in][1 + i] = (uint8_t)(o >> (8 * (4 - i))); }
        nroom_in++;
    }
    pthread_mutex_unlock(&lock);
}
