/* olsc:u (save data cloud backup, nn::olsc), answering like a console that is offline. Up to 2.0.6 the game's nn::olsc
   calls were exported names and were replaced in acnh_offline.c; ACNH 3.0.0 links nn::olsc into main without names, so
   its calls reach this service instead. The behaviour is the same: the backup setting the game chooses is kept for the
   session, there is no cached backup info and custom data is accepted. Anything that would talk to the server, and any
   command not listed, stops the runtime with its id (ipc_unimplemented), so an online path shows up. Command ids:
   IOlscServiceForApplication. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>

static uint8_t backup_setting;

static void olsc_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: return;                                                                /* Initialize */
    case 13: IPC_OUT(rs, backup_setting); return;                                  /* GetSaveDataBackupSetting(uid) */
    case 14: backup_setting = IPC_ARG(rq, uint8_t, 0) ? 1 : 0;                     /* SetSaveDataBackupSettingEnabled(enabled, uid) */
        fprintf(stderr, "[acnh] olsc: save data backup setting %s (kept for this session; offline)\n", backup_setting ? "on" : "off");
        return;
    case 15: return;                                                               /* SetCustomData(uid, buffer): accepted */
    case 18: rs->result = 0x5bba; return;                                          /* GetSaveDataBackupInfoCache: none (not found) */
    default: ipc_unimplemented(s, rq);
    }
}
__attribute__((constructor)) static void register_olsc(void) { runtime_ipc_register("olsc:u", olsc_root, NULL, NULL); }
