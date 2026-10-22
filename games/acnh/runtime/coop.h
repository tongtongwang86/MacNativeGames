/* Local play over IP (docs/LAN-COOP-DESIGN.md sections 6-7). The game sees a virtual LDN subnet 10.114.0.0/24 where
   node n (0 = the host) is 10.114.0.(n+1); the runtime maps those addresses to real peers on a LAN, ZeroTier or
   loopback (two copies on one Mac, told apart by their port offset).
     ldn_host.c    nn::ldn by name: the state machine and NetworkInfo the game reads
     socket_host.c nn::socket by name: host sockets, virtual addresses, Linux errno numbers
     coop_lan.c    discovery (UDP) and session control (TCP) between copies; the island browser's list */
#ifndef ACNH_COOP_H
#define ACNH_COOP_H
#include <stddef.h>
#include <stdint.h>

#define COOP_SUBNET 0x0a720000u        /* 10.114.0.0, host byte order (nn::ldn::Ipv4Address) */
#define COOP_MASK 0xffffff00u
#define COOP_BROADCAST (COOP_SUBNET | 0xffu)
#define COOP_NODES 8
#define COOP_PORT 11452                /* discovery (UDP) and control (TCP) port, plus this copy's port offset */

/* nn::ldn layouts (libnx ldn.h; sizes checked in ldn_host.c) */
typedef struct { uint64_t local_communication_id; uint8_t reserved0[2]; uint16_t scene_id; uint8_t reserved1[4]; } LdnIntentId;
typedef struct { uint8_t bytes[16]; } LdnSessionId;
typedef struct { LdnIntentId intent; LdnSessionId session; } LdnNetworkId;
typedef struct { uint8_t length; char raw[33]; } LdnSsid;
typedef struct {
    uint32_t ipv4; uint8_t mac[6]; int8_t node_id; uint8_t is_connected; char user_name[33]; uint8_t reserved0;
    int16_t local_communication_version; uint8_t reserved1[16];
} LdnNodeInfo;
typedef struct {
    LdnNetworkId network_id;
    struct { uint8_t bssid[6]; LdnSsid ssid; int16_t channel; int8_t link_level; uint8_t network_type; uint32_t reserved; } common;
    struct {
        uint8_t security_parameter[16]; uint16_t security_mode; uint8_t station_accept_policy; uint8_t reserved0[3];
        uint8_t node_count_max; uint8_t node_count; LdnNodeInfo nodes[COOP_NODES]; uint16_t reserved1;
        uint16_t advertise_data_size; uint8_t advertise_data[0x180]; uint8_t reserved2[0x8c]; uint64_t authentication_id;
    } ldn;
} LdnNetworkInfo;

/* One island the browser knows: found by discovery or added by address. */
typedef struct {
    char address[64];          /* "a.b.c.d:port" of its control port */
    char island[48], player[34];
    int players, max_players;
    uint64_t seen_ns;          /* 0: added by hand and not answering */
    int manual;
} CoopIsland;

/* coop_lan.c */
int coop_port_offset(void);
void coop_lan_host_start(const LdnNetworkInfo *info);       /* ldn CreateNetwork: answer scans, accept stations */
void coop_lan_host_update(const LdnNetworkInfo *info);      /* advertise data or node list changed: push to stations */
void coop_lan_host_stop(void);
void coop_lan_host_reject(uint32_t virtual_ipv4);
int coop_lan_scan(LdnNetworkInfo *out, int max, unsigned wait_ms);   /* the game's Scan: islands answering now */
int coop_lan_connect(const LdnNetworkInfo *target, const char *user_name, int16_t version, LdnNetworkInfo *joined, int *node);
void coop_lan_disconnect(void);
/* Where node n's traffic goes: its real IPv4 (network order) and port offset. 0 when node n is not in the session. */
int coop_lan_node_route(int node, uint32_t *ip_be, int *offset);
int coop_lan_self_node(void);   /* this copy's node, -1 outside a session */
/* Island browser (Settings > Local play): the list, manual entries, and the island the game's scan should return. */
int coop_browser_list(CoopIsland *out, int max);
void coop_browser_add(const char *address);
void coop_browser_remove(const char *address);
void coop_browser_select(const char *address);   /* NULL or "": the game's scan lists every island found */
const char *coop_browser_selected(void);
void coop_browser_refresh(void);
const char *coop_status(void);                    /* one line for the browser: hosting / visiting / idle */

/* ldn_host.c: called by coop_lan.c from its network thread */
void ldn_host_station_joined(int node, const char *user_name, int16_t version);
void ldn_host_station_left(int node);
void ldn_host_network_updated(const LdnNetworkInfo *info);   /* station: the host sent a new NetworkInfo */
void ldn_host_lost_host(void);                                /* station: the host went away */
/* coop_flow.c: Local play from anywhere (1 visit, 2 host); started at the next controller read on the game's thread */
void coop_flow_request(int what);
const char *coop_flow_result(void);
void *runtime_ldn_override(const char *name);
void *runtime_socket_override(const char *name);
#endif
