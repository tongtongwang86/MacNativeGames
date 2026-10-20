/* pctl (parental controls), answering like a console with no restrictions set. ACNH 3.0.0 asks after the title screen
   (2.0.6 did not reach it in this port). Restriction checks pass and report "not restricted"; any command not listed
   stops the runtime with its id (ipc_unimplemented). Command ids: IParentalControlServiceFactory / IParentalControlService. */
#include "runtime.h"
#include "ipc.h"

static void service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: return;                                                       /* Initialize */
    case 1001: case 1002: case 1003: case 1004: case 1007: case 1013:     /* Check / Confirm ... Permission: allowed */
    case 1017: case 1018:                                                 /* EndFreeCommunication, IsFreeCommunicationAvailable */
        return;
    case 1006: case 1031: case 1035: { uint8_t no = 0; IPC_OUT(rs, no); return; }   /* IsRestricted..., IsRestrictionEnabled, ... */
    case 1032: { uint32_t level = 0; IPC_OUT(rs, level); return; }      /* GetSafetyLevel: none */
    default: ipc_unimplemented(s, rq);
    }
}
static void factory(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd == 0 || rq->cmd == 1)   /* CreateService, CreateServiceWithoutInitialize */
        rs->move[rs->nmove++] = runtime_ipc_new_session("pctl IParentalControlService", service, NULL, NULL);
    else ipc_unimplemented(s, rq);
}
__attribute__((constructor)) static void register_pctl(void) {
    runtime_ipc_register("pctl", factory, NULL, NULL);
    runtime_ipc_register("pctl:a", factory, NULL, NULL);
    runtime_ipc_register("pctl:s", factory, NULL, NULL);
    runtime_ipc_register("pctl:r", factory, NULL, NULL);
}
