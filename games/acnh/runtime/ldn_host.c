/* nn::ldn (local wireless) replaced by name, so the game's "Local Play" runs over IP (coop.h, coop_lan.c).
   main calls these 25 functions directly; nothing reaches ldn:u. The state machine follows the SDK's:
   None -> Initialized -> AccessPoint -> AccessPointCreated (host) / Station -> StationConnected (visitor),
   and every change signals the SystemEvent the game attached. Node n has the virtual address 10.114.0.(n+1). */
#include "runtime.h"
#include "coop.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(LdnIntentId) == 0x10, "IntentId");
_Static_assert(sizeof(LdnNodeInfo) == 0x40, "NodeInfo");
_Static_assert(sizeof(LdnNetworkInfo) == 0x480, "NetworkInfo");
_Static_assert(offsetof(LdnNetworkInfo, ldn) == 0x50, "NetworkInfo.ldn");
_Static_assert(offsetof(LdnNetworkInfo, ldn.nodes) == 0x68, "NetworkInfo.ldn.nodes");
_Static_assert(offsetof(LdnNetworkInfo, ldn.advertise_data) == 0x26c, "NetworkInfo.ldn.advertise_data");

typedef struct { LdnIntentId intent; uint16_t channel; uint8_t node_count_max; uint8_t reserved0; int16_t local_communication_version; uint8_t reserved1[10]; } LdnNetworkConfig;
typedef struct { uint16_t security_mode; uint16_t passphrase_size; uint8_t passphrase[0x40]; } LdnSecurityConfig;
typedef struct { uint8_t data[16]; LdnSessionId session; } LdnSecurityParameter;
typedef struct { char user_name[33]; uint8_t reserved[15]; } LdnUserConfig;
typedef struct { LdnNetworkId network_id; uint32_t network_type; uint8_t bssid[6]; LdnSsid ssid; uint8_t reserved[16]; uint32_t flag; } LdnScanFilter;
_Static_assert(sizeof(LdnNetworkConfig) == 0x20, "NetworkConfig");
_Static_assert(sizeof(LdnSecurityConfig) == 0x44, "SecurityConfig");
_Static_assert(sizeof(LdnSecurityParameter) == 0x20, "SecurityParameter");
_Static_assert(sizeof(LdnUserConfig) == 0x30, "UserConfig");
_Static_assert(sizeof(LdnScanFilter) == 0x60, "ScanFilter");

enum { ST_NONE, ST_INITIALIZED, ST_ACCESS_POINT, ST_ACCESS_POINT_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
enum { REASON_NONE, REASON_USER, REASON_SYSTEM, REASON_DESTROYED_BY_USER, REASON_DESTROYED_BY_SYSTEM, REASON_ADMIN, REASON_SIGNAL_LOST };
enum { FILTER_LOCAL_ID = 1, FILTER_SESSION_ID = 2, FILTER_NETWORK_TYPE = 4, FILTER_BSSID = 8, FILTER_SSID = 16, FILTER_SCENE_ID = 32 };
#define LDN_RESULT(d) (203u | ((uint32_t)(d) << 9))
#define R_INVALID_STATE LDN_RESULT(32)
#define R_CONNECT_FAILURE LDN_RESULT(64)
#define R_CONNECT_NOT_FOUND LDN_RESULT(65)
#define R_INVALID_ARGUMENT LDN_RESULT(96)

void hle_signal_system_event(void *event); void hle_unbind_system_event(void *event);   /* threads.c */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state, reason, self_node = -1;
static void *state_event;
static LdnNetworkInfo info;              /* the session's NetworkInfo: ours as host, the host's as station */
static LdnNetworkInfo last_scan[16]; static int last_scan_count;
/* The host sends its updated NetworkInfo (with the new station in it) right after accepting; it can arrive before
   connect_info marks the station connected, so it is kept here and used when the join completes. */
static LdnNetworkInfo pending; static int have_pending;

static const char *state_name(int s) {
    static const char *n[] = {"None", "Initialized", "AccessPoint", "AccessPointCreated", "Station", "StationConnected", "Error"};
    return s >= 0 && s <= ST_ERROR ? n[s] : "?";
}
static void set_state(int s) {   /* lock held */
    if (s != state) fprintf(stderr, "[ldn] %s -> %s\n", state_name(state), state_name(s));
    state = s;
    if (state_event) hle_signal_system_event(state_event);
}
static int trace_reads(void) { static int t = -1; if (t < 0) t = getenv("ACNH_COOP_TRACE") != NULL; return t; }
static void signal_change(void) {
    if (trace_reads()) fprintf(stderr, "[ldn] state change event %s\n", state_event ? "signalled" : "not attached");
    if (state_event) hle_signal_system_event(state_event);
}

static uint32_t initialize(void) { pthread_mutex_lock(&lock); if (state == ST_NONE) set_state(ST_INITIALIZED); pthread_mutex_unlock(&lock); return 0; }
static void leave_network(void) {   /* lock held */
    if (state == ST_ACCESS_POINT_CREATED) coop_lan_host_stop();
    if (state == ST_STATION_CONNECTED) coop_lan_disconnect();
    self_node = -1;
}
/* The game destroys the attached event around Finalize, so it is detached here, not signalled. */
static uint32_t finalize(void) {
    pthread_mutex_lock(&lock); leave_network();
    if (state_event) hle_unbind_system_event(state_event);
    state_event = NULL; fprintf(stderr, "[ldn] %s -> None\n", state_name(state)); state = ST_NONE;
    pthread_mutex_unlock(&lock); return 0;
}
static int get_state(void) {
    pthread_mutex_lock(&lock); int s = state; pthread_mutex_unlock(&lock);
    static int last = -1; if (trace_reads() && s != last) { fprintf(stderr, "[ldn] GetState -> %s\n", state_name(s)); last = s; }
    return s;
}
static int disconnect_reason(void) { pthread_mutex_lock(&lock); int r = reason; pthread_mutex_unlock(&lock); return r; }
static void attach_state_change_event(void *event) {
    if (!event) runtime_fail("ldn state change event");
    hle_unbind_system_event(event);    /* a new event may reuse an old one's memory */
    hle_bind_system_event(event, 1);   /* on the game's thread: the SDK creates the event */
    pthread_mutex_lock(&lock); state_event = event; pthread_mutex_unlock(&lock);
}
static uint32_t make_broadcast(uint32_t ip, uint32_t mask) { return ip | ~mask; }

static uint32_t open_access_point(void) {
    pthread_mutex_lock(&lock); uint32_t r = 0;
    if (state == ST_INITIALIZED || state == ST_ACCESS_POINT) set_state(ST_ACCESS_POINT); else r = R_INVALID_STATE;
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t close_access_point(void) {
    pthread_mutex_lock(&lock); uint32_t r = 0;
    if (state == ST_ACCESS_POINT || state == ST_ACCESS_POINT_CREATED) { leave_network(); set_state(ST_INITIALIZED); } else r = R_INVALID_STATE;
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t create(const LdnNetworkConfig *config, const LdnSecurityConfig *security, const LdnSecurityParameter *param, const LdnUserConfig *user) {
    pthread_mutex_lock(&lock);
    if (state != ST_ACCESS_POINT) { pthread_mutex_unlock(&lock); return R_INVALID_STATE; }
    uint16_t keep_size = info.ldn.advertise_data_size; uint8_t keep[0x180]; memcpy(keep, info.ldn.advertise_data, sizeof keep);
    memset(&info, 0, sizeof info);
    info.network_id.intent = config->intent;
    if (param) { info.network_id.session = param->session; memcpy(info.ldn.security_parameter, param->data, 16); }
    else { arc4random_buf(&info.network_id.session, 16); arc4random_buf(info.ldn.security_parameter, 16); }
    arc4random_buf(info.common.bssid, 6); info.common.bssid[0] = (uint8_t)((info.common.bssid[0] & 0xfe) | 2);
    info.common.ssid.length = 32;
    for (int i = 0; i < 32; i++) info.common.ssid.raw[i] = "0123456789abcdef"[arc4random_uniform(16)];
    info.common.channel = config->channel ? (int16_t)config->channel : 6;
    info.common.link_level = 3; info.common.network_type = 2;
    info.ldn.security_mode = security->security_mode;
    info.ldn.node_count_max = config->node_count_max && config->node_count_max <= COOP_NODES ? config->node_count_max : COOP_NODES;
    info.ldn.node_count = 1;
    LdnNodeInfo *n = &info.ldn.nodes[0];
    n->ipv4 = COOP_SUBNET | 1; memcpy(n->mac, info.common.bssid, 6); n->node_id = 0; n->is_connected = 1;
    memcpy(n->user_name, user->user_name, 32); n->local_communication_version = config->local_communication_version;
    for (int i = 1; i < COOP_NODES; i++) info.ldn.nodes[i].node_id = (int8_t)i;
    info.ldn.advertise_data_size = keep_size; memcpy(info.ldn.advertise_data, keep, sizeof keep);
    self_node = 0; reason = REASON_NONE;
    fprintf(stderr, "[ldn] network created: local id %#llx scene %u, %u players max, user \"%.32s\"\n",
            (unsigned long long)config->intent.local_communication_id, config->intent.scene_id, info.ldn.node_count_max, user->user_name);
    coop_lan_host_start(&info);
    set_state(ST_ACCESS_POINT_CREATED);
    pthread_mutex_unlock(&lock); return 0;
}
static uint32_t create_network(const LdnNetworkConfig *c, const LdnSecurityConfig *s, const LdnUserConfig *u) { return create(c, s, NULL, u); }
static uint32_t create_network_private(const LdnNetworkConfig *c, const LdnSecurityConfig *s, const LdnSecurityParameter *p, const LdnUserConfig *u, int count, const void *entries) {
    (void)count; (void)entries; return create(c, s, p, u);
}
static uint32_t destroy_network(void) {
    pthread_mutex_lock(&lock); uint32_t r = 0;
    if (state == ST_ACCESS_POINT_CREATED) { leave_network(); set_state(ST_ACCESS_POINT); } else r = R_INVALID_STATE;
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t set_advertise_data(const void *data, size_t size) {
    if (size > sizeof info.ldn.advertise_data) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    memcpy(info.ldn.advertise_data, data, size); memset(info.ldn.advertise_data + size, 0, sizeof info.ldn.advertise_data - size);
    info.ldn.advertise_data_size = (uint16_t)size;
    if (state == ST_ACCESS_POINT_CREATED) coop_lan_host_update(&info);
    pthread_mutex_unlock(&lock); return 0;
}
static uint32_t set_accept_policy(uint8_t policy) {
    pthread_mutex_lock(&lock); info.ldn.station_accept_policy = policy;
    if (state == ST_ACCESS_POINT_CREATED) coop_lan_host_update(&info);
    pthread_mutex_unlock(&lock); return 0;
}
static uint32_t reject(uint32_t ip) {
    pthread_mutex_lock(&lock); uint32_t r = state == ST_ACCESS_POINT_CREATED ? 0 : R_INVALID_STATE; pthread_mutex_unlock(&lock);
    if (!r) coop_lan_host_reject(ip);
    return r;
}

static uint32_t open_station(void) {
    pthread_mutex_lock(&lock); uint32_t r = 0;
    if (state == ST_INITIALIZED || state == ST_STATION) set_state(ST_STATION); else r = R_INVALID_STATE;
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t close_station(void) {
    pthread_mutex_lock(&lock); uint32_t r = 0;
    if (state == ST_STATION || state == ST_STATION_CONNECTED) { leave_network(); set_state(ST_INITIALIZED); } else r = R_INVALID_STATE;
    pthread_mutex_unlock(&lock); return r;
}
static int matches(const LdnNetworkInfo *n, const LdnScanFilter *f) {
    if (!f) return 1;
    if ((f->flag & FILTER_LOCAL_ID) && n->network_id.intent.local_communication_id != f->network_id.intent.local_communication_id) return 0;
    if ((f->flag & FILTER_SCENE_ID) && n->network_id.intent.scene_id != f->network_id.intent.scene_id) return 0;
    if ((f->flag & FILTER_SESSION_ID) && memcmp(&n->network_id.session, &f->network_id.session, 16)) return 0;
    if ((f->flag & FILTER_NETWORK_TYPE) && !(f->network_type & n->common.network_type)) return 0;
    if ((f->flag & FILTER_BSSID) && memcmp(n->common.bssid, f->bssid, 6)) return 0;
    return 1;
}
static uint32_t scan(LdnNetworkInfo *out, int *count, int capacity, const LdnScanFilter *filter, int channel) {
    (void)channel;
    pthread_mutex_lock(&lock); int s = state; pthread_mutex_unlock(&lock);
    if (s != ST_STATION && s != ST_ACCESS_POINT && s != ST_ACCESS_POINT_CREATED) return R_INVALID_STATE;
    LdnNetworkInfo found[16];
    int n;
    { __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away(); n = coop_lan_scan(found, 16, 400); }
    int k = 0;
    static int alter = -1; if (alter < 0) alter = getenv("ACNH_COOP_TEST_ALTER_IDS") != NULL;
    for (int i = 0; i < n && k < capacity; i++) if (matches(&found[i], filter)) {
        out[k] = found[i];
        /* diagnostic: two copies of one save advertise the visitor's own island and player IDs (ACNH's advertise data
           +0x18 island ID, +0x34 player ID); change them to see whether the game hides "its own" island */
        if (alter && out[k].ldn.advertise_data_size >= 0x38) { out[k].ldn.advertise_data[0x18] ^= 0x5a; out[k].ldn.advertise_data[0x34] ^= 0x5a; }
        k++;
    }
    pthread_mutex_lock(&lock); memcpy(last_scan, found, sizeof(LdnNetworkInfo) * (size_t)n); last_scan_count = n; pthread_mutex_unlock(&lock);
    if (count) *count = k;
    fprintf(stderr, "[ldn] scan: %d island(s) answering, %d returned\n", n, k);
    return 0;
}
static uint32_t connect_info(const LdnNetworkInfo *target, const LdnUserConfig *user, int version) {
    pthread_mutex_lock(&lock); int s = state; have_pending = 0; pthread_mutex_unlock(&lock);
    if (s != ST_STATION) return R_INVALID_STATE;
    LdnNetworkInfo joined; int node = -1, r;
    { __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away();
      r = coop_lan_connect(target, user->user_name, (int16_t)version, &joined, &node); }
    if (r) { fprintf(stderr, "[ldn] connect failed (%d)\n", r); return r == 2 ? R_CONNECT_NOT_FOUND : R_CONNECT_FAILURE; }
    pthread_mutex_lock(&lock);
    info = have_pending ? pending : joined; have_pending = 0; self_node = node; reason = REASON_NONE;
    set_state(ST_STATION_CONNECTED);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[ldn] joined as node %d (10.114.0.%d)\n", node, node + 1);
    return 0;
}
static uint32_t connect_network(const LdnNetworkInfo *target, const LdnSecurityConfig *s, const LdnUserConfig *u, int version, int option) {
    (void)s; (void)option; return connect_info(target, u, version);
}
static uint32_t connect_private(const LdnNetworkConfig *c, const LdnSecurityConfig *s, const LdnSecurityParameter *p, const LdnUserConfig *u, int version, int option) {
    (void)c; (void)s; (void)option;
    LdnNetworkInfo target; int found = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < last_scan_count && !found; i++) if (!memcmp(&last_scan[i].network_id.session, &p->session, 16)) { target = last_scan[i]; found = 1; }
    pthread_mutex_unlock(&lock);
    if (!found) { LdnNetworkInfo all[16]; int n = coop_lan_scan(all, 16, 400);
        for (int i = 0; i < n && !found; i++) if (!memcmp(&all[i].network_id.session, &p->session, 16)) { target = all[i]; found = 1; } }
    if (!found) return R_CONNECT_NOT_FOUND;
    return connect_info(&target, u, version);
}
static uint32_t disconnect(void) {
    pthread_mutex_lock(&lock); uint32_t r = 0;
    if (state == ST_STATION_CONNECTED) { leave_network(); reason = REASON_USER; set_state(ST_STATION); } else r = R_INVALID_STATE;
    pthread_mutex_unlock(&lock); return r;
}

static int in_network(void) { return state == ST_ACCESS_POINT_CREATED || state == ST_STATION_CONNECTED; }
static uint32_t get_network_info(LdnNetworkInfo *out) {
    pthread_mutex_lock(&lock); uint32_t r = in_network() ? 0 : R_INVALID_STATE; if (!r) *out = info;
    static int last = -1; int c = r ? -1 : info.ldn.node_count;
    if (trace_reads() && c != last) {
        fprintf(stderr, "[ldn] GetNetworkInfo -> %d node(s):", c);
        for (int i = 0; !r && i < COOP_NODES; i++) if (info.ldn.nodes[i].is_connected)
            fprintf(stderr, " [%d %08x %.12s v%d]", info.ldn.nodes[i].node_id, info.ldn.nodes[i].ipv4, info.ldn.nodes[i].user_name, info.ldn.nodes[i].local_communication_version);
        fprintf(stderr, "\n"); last = c;
    }
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t get_ipv4(uint32_t *ip, uint32_t *mask) {
    pthread_mutex_lock(&lock); uint32_t r = in_network() ? 0 : R_INVALID_STATE;
    fprintf(stderr, "[ldn] GetIpv4Address -> %s node %d\n", r ? "not in a network" : "ok", self_node);
    if (!r) { if (ip) *ip = COOP_SUBNET | (uint32_t)(self_node + 1); if (mask) *mask = COOP_MASK; }
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t get_security_parameter(LdnSecurityParameter *out) {
    pthread_mutex_lock(&lock); uint32_t r = in_network() ? 0 : R_INVALID_STATE;
    if (!r) { memcpy(out->data, info.ldn.security_parameter, 16); out->session = info.network_id.session; }
    pthread_mutex_unlock(&lock); return r;
}
static uint32_t get_network_config(LdnNetworkConfig *out) {
    pthread_mutex_lock(&lock); uint32_t r = in_network() ? 0 : R_INVALID_STATE;
    if (!r) { memset(out, 0, sizeof *out); out->intent = info.network_id.intent; out->channel = (uint16_t)info.common.channel;
        out->node_count_max = info.ldn.node_count_max; out->local_communication_version = info.ldn.nodes[0].local_communication_version; }
    pthread_mutex_unlock(&lock); return r;
}

/* ---------- from coop_lan.c's network thread ---------- */
void ldn_host_station_joined(int node, const char *user_name, int16_t version) {
    pthread_mutex_lock(&lock);
    if (state == ST_ACCESS_POINT_CREATED && node > 0 && node < COOP_NODES) {
        LdnNodeInfo *n = &info.ldn.nodes[node];
        memset(n, 0, sizeof *n); n->ipv4 = COOP_SUBNET | (uint32_t)(node + 1); n->node_id = (int8_t)node; n->is_connected = 1;
        arc4random_buf(n->mac, 6); n->mac[0] = (uint8_t)((n->mac[0] & 0xfe) | 2);
        snprintf(n->user_name, sizeof n->user_name, "%s", user_name); n->local_communication_version = version;
        int c = 0; for (int i = 0; i < COOP_NODES; i++) c += info.ldn.nodes[i].is_connected; info.ldn.node_count = (uint8_t)c;
        coop_lan_host_update(&info); signal_change();
    }
    pthread_mutex_unlock(&lock);
}
void ldn_host_station_left(int node) {
    pthread_mutex_lock(&lock);
    if (state == ST_ACCESS_POINT_CREATED && node > 0 && node < COOP_NODES && info.ldn.nodes[node].is_connected) {
        LdnNodeInfo *n = &info.ldn.nodes[node]; memset(n, 0, sizeof *n); n->node_id = (int8_t)node;
        int c = 0; for (int i = 0; i < COOP_NODES; i++) c += info.ldn.nodes[i].is_connected; info.ldn.node_count = (uint8_t)c;
        coop_lan_host_update(&info); signal_change();
    }
    pthread_mutex_unlock(&lock);
}
void ldn_host_network_updated(const LdnNetworkInfo *n) {
    pthread_mutex_lock(&lock);
    if (state == ST_STATION_CONNECTED) { info = *n; signal_change(); }
    else if (state == ST_STATION) { pending = *n; have_pending = 1; }
    pthread_mutex_unlock(&lock);
}
void ldn_host_lost_host(void) {
    pthread_mutex_lock(&lock);
    if (state == ST_STATION_CONNECTED) { self_node = -1; reason = REASON_DESTROYED_BY_USER; set_state(ST_STATION); }
    pthread_mutex_unlock(&lock);
}

void *runtime_ldn_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn3ldn10InitializeEv", initialize}, {"_ZN2nn3ldn8FinalizeEv", finalize}, {"_ZN2nn3ldn8GetStateEv", get_state},
        {"_ZN2nn3ldn19GetDisconnectReasonEv", disconnect_reason},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", attach_state_change_event},
        {"_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE", make_broadcast},
        {"_ZN2nn3ldn15OpenAccessPointEv", open_access_point}, {"_ZN2nn3ldn16CloseAccessPointEv", close_access_point},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", create_network},
        {"_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", create_network_private},
        {"_ZN2nn3ldn14DestroyNetworkEv", destroy_network}, {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", set_advertise_data},
        {"_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", set_accept_policy}, {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", reject},
        {"_ZN2nn3ldn11OpenStationEv", open_station}, {"_ZN2nn3ldn12CloseStationEv", close_station},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", scan},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", scan},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", connect_network},
        {"_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", connect_private},
        {"_ZN2nn3ldn10DisconnectEv", disconnect}, {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", get_network_info},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", get_ipv4},
        {"_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", get_security_parameter},
        {"_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", get_network_config},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
