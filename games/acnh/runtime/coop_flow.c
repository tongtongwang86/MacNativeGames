/* Local play without the airport (docs/LAN-COOP-DESIGN.md section 7): the Local play buttons start the game's own
   EventFlow steps Orville would run, from wherever the player is. scripts/make_coop_flows.py writes the flowcharts
   Coop_LocalVisitor (entry Coop_Visit) and Coop_LocalHost (entry Coop_Host) into the RomFS overlay build/romfs-coop
   (ACNH_COOP_ROMFS, romfs.c).

   Starting a script: main+0x107fed0(manager, &request) queues an event, as the game does for System_GameClose
   (main+0x1c63760). manager = **(main+0x4ab21d0); the request is two sead::SafeStrings (vtable *(main+0x4ab0fb0) + 0x10)
   for the flowchart and the entry point, then u32 actor (+0x20, 0), u8 player (+0x24, 0xff), u8 (+0x25), and a pointer to
   the requester's status block (+0x28; the game reads +8 of it: 1 running, 2 finished). (main+0x10805a0 with the same
   request runs a short check script synchronously on the stack, e.g. NNPC_Talk_Situation / Check_ConvReaction: not for
   events.) It runs on the game's main thread: coop_flow_tick is called from the per-frame controller read (input.c). */
#include "runtime.h"
#include "coop.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define START_FLOW 0x107fed0
#define START_FLOW_WORD 0xa9ba7bfdu   /* stp x29, x30, [sp, #-0x60]! (3.0.0) */
#define MANAGER_SLOT 0x4ab21d0
#define SAFE_STRING_VTABLE_SLOT 0x4ab0fb0

typedef struct { uint64_t vtable; const char *str; } SafeString;
typedef struct { SafeString flowchart, entry; uint32_t actor; uint8_t player, flag; uint8_t pad[2]; void *status; } FlowRequest;
_Static_assert(sizeof(FlowRequest) == 0x30, "flow request");

static char last_result[96] = "";
static _Atomic int pending;   /* 0 none, 1 visit, 2 host, 3 test, 4 leave (visitor), 5 stop hosting */
static char test_override[128];
static uint32_t actor_override; static int player_override = -1;   /* "coop actor <hex> <player>" (diagnostics) */
void coop_flow_actor(uint32_t actor, int player) { actor_override = actor; player_override = player; fprintf(stderr, "[coop] request actor %#x player %d\n", actor, player); }
void coop_flow_test(const char *flow_entry) { snprintf(test_override, sizeof test_override, "%s", flow_entry); atomic_store(&pending, 3); }

static const char *const what_name[] = {"", "visit", "host", "test", "leave", "stop hosting"};
static _Atomic uint64_t stop_after_leave;   /* Stop Hosting asked the visitors to leave at this time (ns); 0 none */
void coop_flow_request(int what) {
    int self = coop_lan_self_node();
    /* Leave and stop only inside a session, as the visitor and as the host. */
    if (what == 4 || what == 5) {
        if (self < 0 || (what == 4) != (self > 0)) {
            snprintf(last_result, sizeof last_result, "%s", self < 0 ? "Not in a local play session" : what == 4 ? "You are hosting: use Stop Hosting" : "You are visiting: use Leave Island");
            fprintf(stderr, "[coop] %s request ignored: %s\n", what_name[what], last_result);
            return;
        }
        if (what == 5 && coop_lan_host_visitors(0) > 0) {   /* visitors first fly home themselves, then the gate closes */
            int n = coop_lan_host_visitors(1);
            stop_after_leave = runtime_now_ns();
            snprintf(last_result, sizeof last_result, "Sending %d visitor%s home, then closing the gate", n, n == 1 ? "" : "s");
            fprintf(stderr, "[coop] stop hosting: %s\n", last_result);
            return;
        }
        atomic_store(&pending, what);
        fprintf(stderr, "[coop] %s requested from the Local play menu\n", what_name[what]);
        return;
    }
    /* Running the steps again inside a session restarts local play and breaks it for everyone (the game's own
       airport never offers it then): refused until the session ends. */
    if (self >= 0) {
        snprintf(last_result, sizeof last_result, "%s", coop_lan_self_node() == 0 ? "Already hosting: close the gate at the airport to stop"
                                                                                : "Already on an island: leave it first");
        fprintf(stderr, "[coop] %s request ignored: %s\n", what_name[what], last_result);
        return;
    }
    atomic_store(&pending, what);
    fprintf(stderr, "[coop] %s requested from the Local play menu\n", what_name[what]);
}
const char *coop_flow_result(void) { return last_result; }

void coop_flow_tick(void) {
    coop_edit_tick();   /* coop_edit.c: museum donations between visitor and host */
    uint64_t asked = stop_after_leave;
    if (asked) {   /* Stop Hosting: close the gate once the visitors have gone (they save and fly home first) */
        if (coop_lan_self_node() != 0) stop_after_leave = 0;
        else if (coop_lan_host_visitors(0) == 0) { stop_after_leave = 0; atomic_store(&pending, 5); fprintf(stderr, "[coop] stop hosting: visitors gone, closing the gate\n"); }
        else if (runtime_now_ns() - asked > 120000000000ull) {
            stop_after_leave = 0;
            snprintf(last_result, sizeof last_result, "A visitor did not leave within 2 minutes: the gate stays open");
            fprintf(stderr, "[coop] stop hosting: %s\n", last_result);
        }
    }
    int what = atomic_exchange(&pending, 0);
    if (!what) return;
    uint8_t *main = runtime_module_base("main");
    uint32_t word; memcpy(&word, main + START_FLOW, 4);
    /* ACNH_SITE_PROBE / ACNH_TRACE_SITE may have replaced the word with a branch to their stub or a brk (diagnostics) */
    int diag = (word & 0xfc000000u) == 0x14000000u || word == 0xd4200f40u;
    if (word != START_FLOW_WORD && !diag) { snprintf(last_result, sizeof last_result, "not available in this game version"); fprintf(stderr, "[coop] %s\n", last_result); return; }
    uint64_t manager_slot; memcpy(&manager_slot, main + MANAGER_SLOT, 8);
    uint64_t manager = manager_slot ? *(uint64_t *)(uintptr_t)manager_slot : 0;
    uint64_t vtable; memcpy(&vtable, main + SAFE_STRING_VTABLE_SLOT, 8);
    if (!manager || !vtable) { snprintf(last_result, sizeof last_result, "the game is not ready yet"); return; }
    static FlowRequest rq;
    static uint64_t status[8];   /* the requester's status block (main+0x1c6373c passes its own) */
    memset(status, 0, sizeof status);
    /* what 3 (diagnostics, "coop test"): ACNH_COOP_TEST_FLOW=<flowchart>:<entry> */
    static char test_flow[64], test_entry[64];
    const char *t = test_override[0] ? test_override : getenv("ACNH_COOP_TEST_FLOW");
    if (what == 3) { if (!t || !strchr(t, ':')) return; snprintf(test_flow, sizeof test_flow, "%.*s", (int)(strchr(t, ':') - t), t); snprintf(test_entry, sizeof test_entry, "%s", strchr(t, ':') + 1); }
    const char *e = getenv("ACNH_COOP_FLOW_ACTOR");
    rq = (FlowRequest){
        .flowchart = {vtable + 0x10, what == 1 ? "Coop_LocalVisitor" : what == 2 ? "Coop_LocalHost" : what == 4 ? "Coop_LocalLeave" : what == 5 ? "Coop_LocalClose" : test_flow},
        .entry = {vtable + 0x10, what == 1 ? "Coop_Visit" : what == 2 ? "Coop_Host" : what == 4 ? "Coop_Leave" : what == 5 ? "Coop_Close" : test_entry},
        .actor = actor_override ? actor_override : e ? (uint32_t)strtoul(e, NULL, 0) : 0,
        /* the player the steps act for: this copy's own. Outside a session that is player 0; in one it is this copy's
           node (a visitor's leave as player 0, the host, fails its session lock). 0xff (no player) makes the Net
           actions do nothing. */
        .player = player_override >= 0 ? (uint8_t)player_override : coop_lan_self_node() > 0 ? (uint8_t)coop_lan_self_node() : 0, .status = status};
    ((void (*)(uint64_t, FlowRequest *))(uintptr_t)(main + START_FLOW))(manager, &rq);
    snprintf(last_result, sizeof last_result, "%s requested", what == 1 ? "Visit" : what == 2 ? "Host" : what == 4 ? "Leave" : what == 5 ? "Stop hosting" : "Test");
    fprintf(stderr, "[coop] %s (%s / %s)\n", last_result, rq.flowchart.str, rq.entry.str);
}
