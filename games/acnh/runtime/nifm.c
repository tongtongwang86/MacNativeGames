/* nifm:u (network interface manager), answering like a console with no internet connection: ACNH runs offline
   (acnh-recomp docs/PLAN.md). Requests can be created and submitted; they end "free" with NoInternetConnection, which
   the game treats as offline and goes on. Ported from acnh-recomp runtime/services/nifm.c (behaviour checked on ACNH
   2.0.6 there). Local communication for LDN comes later (N5 / M9). Unknown commands stop the runtime. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIFM_NO_INTERNET ((110u) | (300u << 9))   /* ResultNoInternetConnection (module 110, description 300) */
enum { REQ_FREE = 1 };

typedef struct { uint32_t event[2]; uint32_t state; } Request;

static void request_close(void *p) { free(p); }
static void request(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    Request *r = s->state;
    switch (rq->cmd) {
    case 0: IPC_OUT(rs, r->state); return;                                   /* GetRequestState */
    case 1: if (r->state == REQ_FREE) rs->result = NIFM_NO_INTERNET; return;  /* GetResult */
    case 2:                                                                  /* GetSystemEventReadableHandles */
        rs->copy[rs->ncopy++] = runtime_event_readable(r->event[0]);
        rs->copy[rs->ncopy++] = runtime_event_readable(r->event[1]);
        return;
    case 3: case 4:                                                          /* Cancel, Submit: done at once, not accepted */
        r->state = REQ_FREE; runtime_event_signal(r->event[0]); runtime_event_signal(r->event[1]);
        return;
    case 19: { uint8_t requirement[0x24] = {0}; ipc_out(rs, requirement, sizeof requirement); return; }   /* GetRequirement */
    case 20: case 21: { uint32_t zero = 0; IPC_OUT(rs, zero); return; }       /* GetRevision, GetAppletInfo */
    case 5: case 6: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18:
    case 23: case 24: case 25:
        return;   /* Set* (requirement, priority, persistent, ...), Register/UnregisterSocketDescriptor: accepted */
    default: ipc_unimplemented(s, rq);
    }
}
static uint32_t new_request(void) {
    Request *r = calloc(1, sizeof *r);
    if (!r) runtime_fail("nifm request allocation");
    r->event[0] = runtime_event_new(); r->event[1] = runtime_event_new(); r->state = REQ_FREE;
    return runtime_ipc_new_session("nifm IRequest", request, r, request_close);
}

static void scan_request(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: return;                                    /* Submit, GetResult: done, nothing found */
    case 1: { uint8_t busy = 0; IPC_OUT(rs, busy); return; }   /* IsProcessing */
    case 3: {                                                  /* GetSystemEventReadableHandle: already signalled */
        uint32_t w = runtime_event_new(); runtime_event_signal(w);
        rs->copy[rs->ncopy++] = runtime_event_readable(w);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

static void general_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: {                                                  /* GetClientId -> pointer (C) buffer, u32 */
        if (!rq->nc || rq->c[0].size < 4) runtime_fail("nifm GetClientId buffer");
        uint32_t id = 1; memcpy(ipc_guest(rq->c[0].addr, 4), &id, 4);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, 4, 0};
        return;
    }
    case 2: rs->move[rs->nmove++] = runtime_ipc_new_session("nifm IScanRequest", scan_request, NULL, NULL); return;
    case 4: rs->move[rs->nmove++] = new_request(); return;     /* CreateRequest */
    case 5: case 12: case 18: rs->result = NIFM_NO_INTERNET; return;   /* current profile / IP address / status */
    case 21: case 22: case 23: case 24: { uint8_t no = 0; IPC_OUT(rs, no); return; }   /* Is*Enabled / Is*Accepted */
    default: ipc_unimplemented(s, rq);
    }
}

static void nifm_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd == 4 || rq->cmd == 5)   /* CreateGeneralServiceOld, CreateGeneralService */
        rs->move[rs->nmove++] = runtime_ipc_new_session("nifm IGeneralService", general_service, NULL, NULL);
    else ipc_unimplemented(s, rq);
}
__attribute__((constructor)) static void register_nifm(void) { runtime_ipc_register("nifm:u", nifm_root, NULL, NULL); }
