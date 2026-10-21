/* ACNH's online services, answering like a console that has never been online (the port is offline + local
   wireless). Ported from acnh-recomp (runtime/services/online.c, runtime/hle/ngc.c), where ACNH 2.0.6 went past them:
   - bcat:u: delivery cache storage that mounts and is empty (no news or event data downloaded);
   - friend:u: no friends, presence and play history accepted;
   - nn::ngc::ProfanityFilter: no NG-word list (it lives in a system data archive the port does not have): initialising
     succeeds and nothing is masked.
   Every interface the SDK opens exists: a missing one makes the SDK abort. Commands not listed stop the runtime. */
#include "runtime.h"
#include "ipc.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESULT(module, description) ((uint32_t)(module) | ((uint32_t)(description) << 9))

static uint32_t event_readable(bool signalled) {
    uint32_t w = runtime_event_new();
    if (signalled) runtime_event_signal(w);
    return runtime_event_readable(w);
}
static void out_zero(IpcResponse *rs, uint32_t bytes) { uint8_t z[0x200] = {0}; ipc_out(rs, z, bytes); }

/* ---------- bcat:u ---------- */
#define BCAT_NOT_FOUND RESULT(122, 2)
static void bcat_file(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->result = BCAT_NOT_FOUND; return;     /* Open: no files */
    case 2: case 3: out_zero(rs, 8); return;          /* GetSize, GetDigest */
    default: ipc_unimplemented(s, rq);
    }
}
static void bcat_directory(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->result = BCAT_NOT_FOUND; return;     /* Open: no directories */
    case 1: case 2: out_zero(rs, 4); return;          /* Read, GetCount */
    default: ipc_unimplemented(s, rq);
    }
}
static void bcat_storage(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->move[rs->nmove++] = runtime_ipc_new_session("IDeliveryCacheFileService", bcat_file, NULL, NULL); return;
    case 1: rs->move[rs->nmove++] = runtime_ipc_new_session("IDeliveryCacheDirectoryService", bcat_directory, NULL, NULL); return;
    case 10: out_zero(rs, 4); return;                 /* EnumerateDeliveryCacheDirectory: none */
    default: ipc_unimplemented(s, rq);
    }
}
static void bcat_progress(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->copy[rs->ncopy++] = event_readable(true); return;   /* GetEvent: already done */
    case 1: out_zero(rs, 0x200); return;                             /* GetImpl: nothing synchronised */
    default: ipc_unimplemented(s, rq);
    }
}
static void bcat_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 10100: case 20100: case 20101:               /* RequestSyncDeliveryCache(...) */
        rs->move[rs->nmove++] = runtime_ipc_new_session("IDeliveryCacheProgressService", bcat_progress, NULL, NULL);
        return;
    default: ipc_unimplemented(s, rq);
    }
}
static void bcat_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->move[rs->nmove++] = runtime_ipc_new_session("IBcatService", bcat_service, NULL, NULL); return;
    case 1: case 2: rs->move[rs->nmove++] = runtime_ipc_new_session("IDeliveryCacheStorageService", bcat_storage, NULL, NULL); return;
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- friend:u ---------- */
static void friend_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->copy[rs->ncopy++] = event_readable(false); return;   /* GetCompletionEvent */
    case 10100: case 10101: case 10102: case 10400: case 20102:      /* friend / blocked lists: empty */
        out_zero(rs, 4); return;
    case 10700: case 10701: {   /* GetPlayHistoryRegistrationKey(...WithNetworkServiceAccountId): local play swaps these */
        if (!rq->nc || rq->c[0].size < 0x40) runtime_fail("friend GetPlayHistoryRegistrationKey buffer");
        static uint8_t key[0x40];
        if (!key[0]) { key[0] = 1; key[1] = 1; arc4random_buf(key + 8, sizeof key - 8); }   /* one key per run */
        memcpy(ipc_guest(rq->c[0].addr, sizeof key), key, sizeof key);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, sizeof key, 0};
        return;
    }
    case 10702: case 10703:   /* AddPlayHistory: the islands met in local play are not kept */
    case 1: case 10110: case 10120: case 10121: case 10200: case 10211: case 10600: case 10601: case 10610:
    case 20401: case 20701: case 20800: case 20801: case 20900: case 30100: case 30101: case 30110:
        return;   /* Cancel, presence, play sessions, play history, receipts: accepted */
    default: ipc_unimplemented(s, rq);
    }
}
static void friend_notifications(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->copy[rs->ncopy++] = event_readable(false); return;   /* GetEvent */
    case 1: case 2: rs->result = RESULT(121, 15); return;           /* Clear / Pop: nothing */
    default: ipc_unimplemented(s, rq);
    }
}
static void friend_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: rs->move[rs->nmove++] = runtime_ipc_new_session("IFriendService", friend_service, NULL, NULL); return;
    case 1: rs->move[rs->nmove++] = runtime_ipc_new_session("INotificationService", friend_notifications, NULL, NULL); return;
    case 2: rs->move[rs->nmove++] = runtime_ipc_new_session("IDaemonSuspendSessionService", friend_service, NULL, NULL); return;
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- fsp-srv ----------
   File access goes through host overrides of nn::fs (fs_host.c), but SDK code that wraps other storage (e.g.
   nn::bcat::MountDeliveryCacheStorage) first asks the file system proxy whether access logging is on. Only that
   query and the process registration are served here. */
static void fsp_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: return;                                                    /* SetCurrentProcess */
    case 1005: { uint32_t mode = 0; IPC_OUT(rs, mode); return; }      /* GetGlobalAccessLogMode: off */
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- csrng: random bytes (pia's session keys for local play) ---------- */
static void csrng_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd != 0) { ipc_unimplemented(s, rq); return; }   /* GetRandomBytes -> B buffer */
    for (unsigned i = 0; i < rq->nb; i++) if (rq->b[i].size) arc4random_buf(ipc_guest(rq->b[i].addr, rq->b[i].size), rq->b[i].size);
    for (unsigned i = 0; i < rq->nc; i++) if (rq->c[i].size) {
        arc4random_buf(ipc_guest(rq->c[i].addr, rq->c[i].size), rq->c[i].size);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, 0};
    }
}

__attribute__((constructor)) static void register_offline(void) {
    runtime_ipc_register("csrng", csrng_root, NULL, NULL);
    runtime_ipc_register("bcat:u", bcat_root, NULL, NULL);
    runtime_ipc_register("friend:u", friend_root, NULL, NULL);
    runtime_ipc_register("fsp-srv", fsp_root, NULL, NULL);
}

/* ---------- nn::ngc::ProfanityFilter ---------- */
static void *ngc_ctor(void *self) { return self; }
static void ngc_dtor(void *self) { (void)self; }
static uint32_t ngc_init(void *self, void *work, size_t size, bool check_language) {
    (void)self; (void)work; (void)size; (void)check_language;
    static int once; if (!once++) fprintf(stderr, "[acnh] ProfanityFilter: no NG-word list, nothing is masked\n");
    return 0;
}
static uint32_t ngc_mask_mode(void *self, int mode) { (void)self; (void)mode; return 0; }
static uint32_t ngc_mask_text(void *self, int *count, uint16_t *text, uint32_t mask) { (void)self; (void)text; (void)mask; if (count) *count = 0; return 0; }

/* ---------- nn::olsc (save data cloud backup) ----------
   Offline console: the backup setting the game chooses is kept for the session (the game turns it on when a resident
   is linked to the account), there is no cached backup info, and custom data is accepted. The calls that would talk to
   the server (upload, download, licence check, list) stop the runtime with their name, so an online path shows up. */
static uint8_t olsc_setting;
static uint32_t olsc_set_enabled(const void *uid, bool enabled) { (void)uid; olsc_setting = enabled; return 0; }
static uint8_t olsc_get_setting(const void *uid) { (void)uid; return olsc_setting; }
static bool olsc_try_info_cache(void *info, const void *uid) { (void)info; (void)uid; return false; }
static void olsc_set_custom_data(const void *uid, const char *data, size_t size) { (void)uid; (void)data; (void)size; }
static void olsc_online(void) { runtime_fail("nn::olsc cloud backup request (online feature, not supported offline)"); }

void *runtime_acnh_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn3ngc15ProfanityFilterC1Ev", ngc_ctor}, {"_ZN2nn3ngc15ProfanityFilterD1Ev", ngc_dtor},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_init},
        {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", ngc_mask_mode},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", ngc_mask_text},
        {"_ZN2nn4olsc31SetSaveDataBackupSettingEnabledERKNS_7account3UidEb", olsc_set_enabled},
        {"_ZN2nn4olsc24GetSaveDataBackupSettingERKNS_7account3UidE", olsc_get_setting},
        {"_ZN2nn4olsc29TryGetSaveDataBackupInfoCacheEPNS0_18SaveDataBackupInfoERKNS_7account3UidE", olsc_try_info_cache},
        {"_ZN2nn4olsc13SetCustomDataERKNS_7account3UidEPKcm", olsc_set_custom_data},
        {"_ZN2nn4olsc25UploadSaveDataBackupAsyncERKNS_7account10UserHandleE", olsc_online},
        {"_ZN2nn4olsc27DownloadSaveDataBackupAsyncERKNS_7account10UserHandleENS0_16SaveDataBackupIdE", olsc_online},
        {"_ZN2nn4olsc32VerifySaveDataBackupLicenseAsyncERKNS_7account10UserHandleE", olsc_online},
        {"_ZN2nn4olsc34UpdateSaveDataBackupInfoCacheAsyncERKNS_7account10UserHandleE", olsc_online},
        {"_ZN2nn4olsc39ListDownloadableSaveDataBackupInfoAsyncERKNS_7account10UserHandleEPvm", olsc_online}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
