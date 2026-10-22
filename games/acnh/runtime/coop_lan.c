/* Discovery and session control between copies of the port (docs/LAN-COOP-DESIGN.md sections 6-7). The port's own
   protocol (not ldn_mitm's): every message starts with "ACNHCOOP" and a type byte.
   - Discovery, UDP: a scanner sends SCAN to every interface's broadcast address, to loopback (other copies on this
     Mac, ports COOP_PORT + 100 k) and to each address in the browser's list; a host answers INFO (its control port,
     island name, NetworkInfo). Works on a LAN and on ZeroTier; on networks without broadcast the address is typed in.
   - Control, TCP on the host's COOP_PORT + offset: a station sends JOIN (name, version, its port offset), the host
     answers ACCEPT (node, NetworkInfo) and then sends every member NODES (where each node's traffic goes) and INFO
     whenever the session changes. A station may send EDIT (coop_edit.c: an edit of the island that only the host's
     copy may carry out, e.g. a museum donation). Closing the connection leaves the session.
   ACNH_COOP_PORT_OFFSET gives a copy its port offset (two copies on one Mac: 0 and 100); ACNH_COOP_PEERS adds
   addresses ("ip[:port],..."). The browser's manual list is <profile>/coop-islands.txt. */
#include "runtime.h"
#include "coop.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { MSG_SCAN = 1, MSG_INFO = 2, MSG_JOIN = 3, MSG_ACCEPT = 4, MSG_NODES = 5, MSG_UPDATE = 6, MSG_REFUSE = 7, MSG_EDIT = 8 };
static const char MAGIC[8] = {'A', 'C', 'N', 'H', 'C', 'O', 'O', 'P'};
#define VERSION 1
#define MAX_ISLANDS 32

typedef struct { int used; uint32_t ip; int offset; } Route;   /* ip in network order */
typedef struct __attribute__((packed)) { char magic[8]; uint8_t type, version; uint16_t control_port; char island[48]; LdnNetworkInfo info; } InfoPacket;
typedef struct __attribute__((packed)) { char user[33]; int16_t version; int32_t offset; } JoinBody;
typedef struct __attribute__((packed)) { uint8_t node; LdnNetworkInfo info; } AcceptBody;
typedef struct __attribute__((packed)) { uint8_t used; uint32_t ip; int32_t offset; } NodeEntry;   /* ip 0: the address you reached the host on */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Route routes[COOP_NODES];
static int self_node = -1;

/* host */
static int hosting, host_gen, host_wake[2] = {-1, -1};
static pthread_t host_thread;
static LdnNetworkInfo host_info;
static struct { int fd; int node; uint32_t ip; int offset; } members[COOP_NODES];
static char island_name[48];

/* station */
static int station_fd = -1, station_wake[2] = {-1, -1};
static pthread_t station_thread;
static int station_running;

/* browser */
static CoopIsland islands[MAX_ISLANDS]; static int nislands;
static LdnNetworkInfo island_info[MAX_ISLANDS];
static char selected[64];
static char status_line[160] = "Local play: idle";

int coop_port_offset(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("ACNH_COOP_PORT_OFFSET"); off = e ? atoi(e) : 0; if (off < 0 || off > 20000) off = 0; }
    return off;
}
static const char *profile_dir(void) { const char *d = getenv("TOTK_PROFILE_DIR"); return d && *d ? d : "user-data"; }
static void set_status(const char *s) { pthread_mutex_lock(&lock); snprintf(status_line, sizeof status_line, "%s", s); pthread_mutex_unlock(&lock); fprintf(stderr, "[coop] %s\n", s); }
const char *coop_status(void) { static char copy[160]; pthread_mutex_lock(&lock); memcpy(copy, status_line, sizeof copy); pthread_mutex_unlock(&lock); return copy; }

/* ---------- framing ---------- */
static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { ssize_t w = send(fd, b, n, 0); if (w < 0) { if (errno == EINTR) continue; return -1; } b += w; n -= (size_t)w; }
    return 0;
}
static int read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) { ssize_t r = recv(fd, b, n, 0); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}
static int send_msg(int fd, uint8_t type, const void *body, uint32_t n) {
    uint8_t h[13]; memcpy(h, MAGIC, 8); h[8] = type; h[9] = (uint8_t)(n >> 24); h[10] = (uint8_t)(n >> 16); h[11] = (uint8_t)(n >> 8); h[12] = (uint8_t)n;
    return write_all(fd, h, sizeof h) || (n && write_all(fd, body, n)) ? -1 : 0;
}
static int recv_msg(int fd, uint8_t *type, void *body, uint32_t cap, uint32_t *n) {
    uint8_t h[13];
    if (read_all(fd, h, sizeof h) || memcmp(h, MAGIC, 8)) return -1;
    uint32_t len = (uint32_t)h[9] << 24 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 8 | h[12];
    if (len > cap) return -1;
    if (len && read_all(fd, body, len)) return -1;
    *type = h[8]; *n = len; return 0;
}
static void set_timeout(int fd, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
static int parse_address(const char *s, struct sockaddr_in *out) {
    char host[64]; int port = COOP_PORT; const char *colon = strrchr(s, ':');
    size_t n = colon ? (size_t)(colon - s) : strlen(s); if (n == 0 || n >= sizeof host) return 0;
    memcpy(host, s, n); host[n] = 0; if (colon) port = atoi(colon + 1);
    memset(out, 0, sizeof *out); out->sin_len = sizeof *out; out->sin_family = AF_INET; out->sin_port = htons((uint16_t)port);
    return port > 0 && port < 65536 && inet_pton(AF_INET, host, &out->sin_addr) == 1;
}
static void format_address(const struct sockaddr_in *a, char *out, size_t n) {
    char ip[16]; inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip); snprintf(out, n, "%s:%u", ip, ntohs(a->sin_port));
}

/* ---------- routes ---------- */
int coop_lan_node_route(int node, uint32_t *ip, int *offset) {
    if (node < 0 || node >= COOP_NODES) return 0;
    pthread_mutex_lock(&lock);
    int ok = self_node >= 0 && routes[node].used;
    if (ok) { *ip = routes[node].ip; *offset = routes[node].offset; }
    pthread_mutex_unlock(&lock);
    return ok;
}
int coop_lan_self_node(void) { pthread_mutex_lock(&lock); int n = self_node; pthread_mutex_unlock(&lock); return n; }
static void set_self(int node) {   /* lock held */
    self_node = node; memset(routes, 0, sizeof routes);
    if (node >= 0) routes[node] = (Route){1, htonl(INADDR_LOOPBACK), coop_port_offset()};
}

/* ---------- island name (shown in other copies' browsers) ---------- */
static void load_island_name(void) {
    char path[1200]; snprintf(path, sizeof path, "%s/save/device/landname.dat", profile_dir());
    FILE *f = fopen(path, "rb"); uint8_t raw[64] = {0}; size_t n = f ? fread(raw, 1, sizeof raw, f) : 0; if (f) fclose(f);
    size_t o = 0;
    for (size_t i = 0; i + 1 < n && o + 4 < sizeof island_name; i += 2) {
        unsigned c = raw[i] | raw[i + 1] << 8; if (!c) break;
        if (c < 0x80) island_name[o++] = (char)c;
        else if (c < 0x800) { island_name[o++] = (char)(0xc0 | c >> 6); island_name[o++] = (char)(0x80 | (c & 0x3f)); }
        else { island_name[o++] = (char)(0xe0 | c >> 12); island_name[o++] = (char)(0x80 | ((c >> 6) & 0x3f)); island_name[o++] = (char)(0x80 | (c & 0x3f)); }
    }
    island_name[o] = 0;
    if (!o) snprintf(island_name, sizeof island_name, "Island");
}

/* ---------- host ---------- */
static void fill_info_packet(InfoPacket *p) {   /* lock held */
    memcpy(p->magic, MAGIC, 8); p->type = MSG_INFO; p->version = VERSION;
    p->control_port = htons((uint16_t)(COOP_PORT + coop_port_offset()));
    memcpy(p->island, island_name, sizeof p->island); p->info = host_info;
}
static void send_nodes_to(int k) {   /* lock held: member k's view of the node table */
    NodeEntry e[COOP_NODES]; memset(e, 0, sizeof e);
    struct sockaddr_in local; socklen_t ll = sizeof local; getsockname(members[k].fd, (struct sockaddr *)&local, &ll);
    struct sockaddr_in peer; socklen_t pl = sizeof peer; getpeername(members[k].fd, (struct sockaddr *)&peer, &pl);
    int peer_loopback = ntohl(peer.sin_addr.s_addr) >> 24 == 127;
    e[0] = (NodeEntry){1, 0, coop_port_offset()};
    for (int i = 0; i < COOP_NODES; i++) if (members[i].fd >= 0) {
        uint32_t ip = members[i].ip;
        /* a copy on the host's own Mac is 127.0.0.1 to the host, but the host's address to a remote member */
        if (ntohl(ip) >> 24 == 127 && !peer_loopback) ip = local.sin_addr.s_addr;
        e[members[i].node] = (NodeEntry){1, ip, members[i].offset};
    }
    send_msg(members[k].fd, MSG_NODES, e, sizeof e);
}
static void broadcast_session(void) {   /* lock held */
    for (int k = 0; k < COOP_NODES; k++) if (members[k].fd >= 0) { send_nodes_to(k); send_msg(members[k].fd, MSG_UPDATE, &host_info, sizeof host_info); }
    for (int i = 0; i < COOP_NODES; i++) if (i) routes[i].used = 0;
    for (int k = 0; k < COOP_NODES; k++) if (members[k].fd >= 0) routes[members[k].node] = (Route){1, members[k].ip, members[k].offset};
    int n = 1; for (int k = 0; k < COOP_NODES; k++) n += members[k].fd >= 0;
    snprintf(status_line, sizeof status_line, "Hosting %s: %d of %d players, port %d", island_name, n, host_info.ldn.node_count_max, COOP_PORT + coop_port_offset());
}
static void drop_member(int k, const char *why) {   /* lock held on entry, released around the ldn callback */
    int node = members[k].node; close(members[k].fd); members[k].fd = -1;
    fprintf(stderr, "[coop] node %d left (%s)\n", node, why);
    broadcast_session();
    pthread_mutex_unlock(&lock); ldn_host_station_left(node); pthread_mutex_lock(&lock);
}
static void accept_station(int listen_fd) {
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int fd = accept(listen_fd, (struct sockaddr *)&from, &fl); if (fd < 0) return;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    set_timeout(fd, 3000);
    uint8_t type; uint32_t n; JoinBody join;
    if (recv_msg(fd, &type, &join, sizeof join, &n) || type != MSG_JOIN || n != sizeof join) { close(fd); return; }
    join.user[32] = 0;
    pthread_mutex_lock(&lock);
    int node = -1, slot = -1;
    if (hosting && !host_info.ldn.station_accept_policy) {
        for (int i = 1; i < host_info.ldn.node_count_max && node < 0; i++) {
            int taken = 0; for (int k = 0; k < COOP_NODES; k++) taken |= members[k].fd >= 0 && members[k].node == i;
            if (!taken) node = i;
        }
        for (int k = 0; k < COOP_NODES && slot < 0; k++) if (members[k].fd < 0) slot = k;
    }
    if (node < 0 || slot < 0) { pthread_mutex_unlock(&lock); send_msg(fd, MSG_REFUSE, NULL, 0); close(fd); fprintf(stderr, "[coop] refused %s (full or closed)\n", join.user); return; }
    members[slot].fd = fd; members[slot].node = node; members[slot].ip = from.sin_addr.s_addr; members[slot].offset = join.offset;
    AcceptBody a; a.node = (uint8_t)node; a.info = host_info;
    send_msg(fd, MSG_ACCEPT, &a, sizeof a);
    set_timeout(fd, 0);
    char where[32]; format_address(&from, where, sizeof where);
    fprintf(stderr, "[coop] %s joined from %s (port offset %d) as node %d\n", join.user, where, join.offset, node);
    broadcast_session();
    pthread_mutex_unlock(&lock);
    ldn_host_station_joined(node, join.user, join.version);   /* updates host_info and sends it to everyone */
}
static void *host_main(void *arg) {
    int gen = (int)(intptr_t)arg;
    int port = COOP_PORT + coop_port_offset(), one = 1;
    int udp = socket(AF_INET, SOCK_DGRAM, 0), tcp = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0}; a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(tcp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    int bound = 0;   /* a previous host thread may still be closing its sockets */
    for (int tries = 0; tries < 20 && !bound; tries++) {
        bound = !bind(udp, (struct sockaddr *)&a, sizeof a) && !bind(tcp, (struct sockaddr *)&a, sizeof a) && !listen(tcp, 8);
        if (!bound) { close(udp); close(tcp); usleep(100000); udp = socket(AF_INET, SOCK_DGRAM, 0); tcp = socket(AF_INET, SOCK_STREAM, 0);
            setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
            setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            setsockopt(tcp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one); }
    }
    if (!bound) {
        char m[120]; snprintf(m, sizeof m, "Local play: cannot listen on port %d (%s)", port, strerror(errno)); set_status(m);
        close(udp); close(tcp); return NULL;
    }
    fprintf(stderr, "[coop] hosting on UDP/TCP port %d\n", port);
    for (;;) {
        struct pollfd p[3 + COOP_NODES]; int k2fd[COOP_NODES], np = 3;
        p[0] = (struct pollfd){host_wake[0], POLLIN, 0}; p[1] = (struct pollfd){udp, POLLIN, 0}; p[2] = (struct pollfd){tcp, POLLIN, 0};
        pthread_mutex_lock(&lock);
        if (!hosting || gen != host_gen) { pthread_mutex_unlock(&lock); break; }
        for (int k = 0; k < COOP_NODES; k++) if (members[k].fd >= 0) { k2fd[np - 3] = k; p[np++] = (struct pollfd){members[k].fd, POLLIN, 0}; }
        pthread_mutex_unlock(&lock);
        if (poll(p, (nfds_t)np, 1000) < 0 && errno != EINTR) break;
        if (p[0].revents) { char c; while (read(host_wake[0], &c, 1) == 1) {} }
        if (p[1].revents & POLLIN) {
            uint8_t buf[64]; struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(udp, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (r >= 9 && !memcmp(buf, MAGIC, 8) && buf[8] == MSG_SCAN) {
                InfoPacket pkt; pthread_mutex_lock(&lock); fill_info_packet(&pkt); pthread_mutex_unlock(&lock);
                sendto(udp, &pkt, sizeof pkt, 0, (struct sockaddr *)&from, fl);
            }
        }
        if (p[2].revents & POLLIN) accept_station(tcp);
        for (int i = 3; i < np; i++) if (p[i].revents) {
            int k = k2fd[i - 3]; uint8_t type; uint32_t n; uint8_t body[16];
            pthread_mutex_lock(&lock);
            if (members[k].fd == p[i].fd) {
                int node = members[k].node;
                pthread_mutex_unlock(&lock);
                int bad = recv_msg(p[i].fd, &type, body, sizeof body, &n);   /* EDIT, or the station closing */
                if (!bad && type == MSG_EDIT) coop_edit_received(node, body, n);
                pthread_mutex_lock(&lock);
                if (bad && members[k].fd == p[i].fd) drop_member(k, "connection closed");
            }
            pthread_mutex_unlock(&lock);
        }
    }
    close(udp); close(tcp);
    pthread_mutex_lock(&lock);   /* a newer session (host_gen moved on) closed these already */
    if (gen == host_gen) for (int k = 0; k < COOP_NODES; k++) if (members[k].fd >= 0) { close(members[k].fd); members[k].fd = -1; }
    pthread_mutex_unlock(&lock);
    return NULL;
}
static void wake(int *pipe_fds) { if (pipe_fds[1] >= 0) { char c = 1; (void)!write(pipe_fds[1], &c, 1); } }
static void make_pipe(int *fds) {
    if (fds[0] >= 0) return;
    if (pipe(fds)) runtime_fail("coop wake pipe");
    fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK);
}
void coop_lan_host_start(const LdnNetworkInfo *info) {
    load_island_name(); make_pipe(host_wake);
    pthread_mutex_lock(&lock);
    if (hosting) { pthread_mutex_unlock(&lock); coop_lan_host_stop(); pthread_mutex_lock(&lock); }
    host_info = *info; hosting = 1; set_self(0); int gen = ++host_gen;
    static int first = 1;
    for (int k = 0; k < COOP_NODES; k++) { if (!first && members[k].fd >= 0) close(members[k].fd); members[k].fd = -1; }
    first = 0;
    snprintf(status_line, sizeof status_line, "Hosting %s: 1 of %d players, port %d", island_name, info->ldn.node_count_max, COOP_PORT + coop_port_offset());
    pthread_mutex_unlock(&lock);
    if (pthread_create(&host_thread, NULL, host_main, (void *)(intptr_t)gen)) runtime_fail("coop host thread");
    pthread_detach(host_thread);
    fprintf(stderr, "[coop] %s\n", coop_status());
}
void coop_lan_host_update(const LdnNetworkInfo *info) {
    pthread_mutex_lock(&lock);
    if (hosting) { host_info = *info; for (int k = 0; k < COOP_NODES; k++) if (members[k].fd >= 0) send_msg(members[k].fd, MSG_UPDATE, &host_info, sizeof host_info); }
    pthread_mutex_unlock(&lock);
}
void coop_lan_host_stop(void) {
    pthread_mutex_lock(&lock); int was = hosting; hosting = 0; set_self(-1); pthread_mutex_unlock(&lock);
    if (!was) return;
    wake(host_wake);   /* host_main closes everything; not joined (see station_main) */
    set_status("Local play: idle (stopped hosting)");
}
void coop_lan_host_reject(uint32_t vip) {
    int node = (int)(vip & 0xff) - 1;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < COOP_NODES; k++) if (members[k].fd >= 0 && members[k].node == node) { shutdown(members[k].fd, SHUT_RDWR); wake(host_wake); }
    pthread_mutex_unlock(&lock);
}

/* ---------- scanning and the browser's list ---------- */
static void load_manual(void) {
    static int done; if (done) return; done = 1;
    char path[1200]; snprintf(path, sizeof path, "%s/coop-islands.txt", profile_dir());
    FILE *f = fopen(path, "r"); char line[128];
    while (f && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n ")] = 0; if (*line && *line != '#') coop_browser_add(line); }
    if (f) fclose(f);
    const char *env = getenv("ACNH_COOP_PEERS");
    if (env) { char copy[512]; snprintf(copy, sizeof copy, "%s", env); for (char *t = strtok(copy, ", "); t; t = strtok(NULL, ", ")) coop_browser_add(t); }
}
static void save_manual(void) {   /* lock held */
    char path[1200]; snprintf(path, sizeof path, "%s/coop-islands.txt", profile_dir());
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "# Local play: islands added by address (ip:port), one per line\n");
    for (int i = 0; i < nislands; i++) if (islands[i].manual) fprintf(f, "%s\n", islands[i].address);
    fclose(f);
}
static int find_island(const char *address) { for (int i = 0; i < nislands; i++) if (!strcmp(islands[i].address, address)) return i; return -1; }
static void send_scan(int fd, uint32_t ip_be, int port) {
    uint8_t m[10]; memcpy(m, MAGIC, 8); m[8] = MSG_SCAN; m[9] = VERSION;
    struct sockaddr_in to = {0}; to.sin_len = sizeof to; to.sin_family = AF_INET; to.sin_addr.s_addr = ip_be; to.sin_port = htons((uint16_t)port);
    sendto(fd, m, sizeof m, 0, (struct sockaddr *)&to, sizeof to);
}
int coop_lan_scan(LdnNetworkInfo *out, int max, unsigned wait_ms) {
    load_manual();
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1; if (fd < 0) return 0;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    int own = coop_port_offset();
    struct ifaddrs *ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && (i->ifa_flags & IFF_UP) && i->ifa_broadaddr)
                send_scan(fd, ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr, COOP_PORT);
        freeifaddrs(ifs);
    }
    send_scan(fd, htonl(INADDR_BROADCAST), COOP_PORT);
    for (int k = 0; k < 4; k++) if (k * 100 != own) send_scan(fd, htonl(INADDR_LOOPBACK), COOP_PORT + k * 100);
    pthread_mutex_lock(&lock);
    struct sockaddr_in manual[MAX_ISLANDS]; int nm = 0;
    for (int i = 0; i < nislands; i++) if (islands[i].manual && parse_address(islands[i].address, &manual[nm])) nm++;
    char want[64]; snprintf(want, sizeof want, "%s", selected);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < nm; i++) sendto(fd, (uint8_t[]){'A', 'C', 'N', 'H', 'C', 'O', 'O', 'P', MSG_SCAN, VERSION}, 10, 0, (struct sockaddr *)&manual[i], sizeof manual[i]);
    uint64_t end = runtime_now_ns() + (uint64_t)wait_ms * 1000000ull; int n = 0;
    for (;;) {
        uint64_t now = runtime_now_ns(); if (now >= end) break;
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, (int)((end - now) / 1000000ull) + 1) <= 0) continue;
        InfoPacket pkt; struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(fd, &pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
        if (r != (ssize_t)sizeof pkt || memcmp(pkt.magic, MAGIC, 8) || pkt.type != MSG_INFO || pkt.version != VERSION) continue;
        pthread_mutex_lock(&lock);   /* our own island answers our own scan: not listed */
        int own = hosting && !memcmp(&pkt.info.network_id.session, &host_info.network_id.session, 16);
        pthread_mutex_unlock(&lock);
        if (own) continue;
        struct sockaddr_in ctl = from; ctl.sin_port = pkt.control_port;
        char address[64]; format_address(&ctl, address, sizeof address);
        pkt.island[47] = 0;
        pthread_mutex_lock(&lock);
        int dup = 0; for (int i = 0; i < n; i++) dup |= !memcmp(&out[i].network_id.session, &pkt.info.network_id.session, 16);
        int i = find_island(address);
        for (int j = 0; j < nislands && i < 0; j++)   /* the same island heard on another interface keeps its first address */
            if (islands[j].seen_ns && !memcmp(&island_info[j].network_id.session, &pkt.info.network_id.session, 16)) i = j;
        if (i >= 0 && strcmp(islands[i].address, address) && !dup && (!*want || !strcmp(want, islands[i].address))) { if (n < max) out[n++] = pkt.info; dup = 1; }
        if (i < 0 && nislands < MAX_ISLANDS) { i = nislands++; memset(&islands[i], 0, sizeof islands[i]); snprintf(islands[i].address, sizeof islands[i].address, "%s", address); }
        if (i >= 0) {
            snprintf(islands[i].island, sizeof islands[i].island, "%s", pkt.island);
            snprintf(islands[i].player, sizeof islands[i].player, "%.32s", pkt.info.ldn.nodes[0].user_name);
            islands[i].players = pkt.info.ldn.node_count; islands[i].max_players = pkt.info.ldn.node_count_max;
            islands[i].seen_ns = runtime_now_ns(); island_info[i] = pkt.info;
        }
        pthread_mutex_unlock(&lock);
        if (!dup && n < max && (!*want || !strcmp(want, address))) out[n++] = pkt.info;
    }
    close(fd);
    return n;
}
int coop_browser_list(CoopIsland *out, int max) {
    pthread_mutex_lock(&lock); int n = nislands < max ? nislands : max; memcpy(out, islands, sizeof *out * (size_t)n); pthread_mutex_unlock(&lock);
    return n;
}
void coop_browser_add(const char *address) {
    struct sockaddr_in a; char norm[64];
    char with_port[64]; snprintf(with_port, sizeof with_port, strchr(address, ':') ? "%s" : "%s:%d", address, COOP_PORT);
    if (!parse_address(with_port, &a)) return;
    format_address(&a, norm, sizeof norm);
    pthread_mutex_lock(&lock);
    int i = find_island(norm);
    if (i < 0 && nislands < MAX_ISLANDS) { i = nislands++; memset(&islands[i], 0, sizeof islands[i]); snprintf(islands[i].address, sizeof islands[i].address, "%s", norm); snprintf(islands[i].island, sizeof islands[i].island, "(not answering yet)"); }
    if (i >= 0 && !islands[i].manual) { islands[i].manual = 1; save_manual(); }
    pthread_mutex_unlock(&lock);
}
void coop_browser_remove(const char *address) {
    pthread_mutex_lock(&lock);
    int i = find_island(address);
    if (i >= 0) { memmove(&islands[i], &islands[i + 1], sizeof *islands * (size_t)(nislands - i - 1)); memmove(&island_info[i], &island_info[i + 1], sizeof *island_info * (size_t)(nislands - i - 1)); nislands--; save_manual(); }
    if (!strcmp(selected, address)) selected[0] = 0;
    pthread_mutex_unlock(&lock);
}
void coop_browser_select(const char *address) {
    pthread_mutex_lock(&lock); snprintf(selected, sizeof selected, "%s", address ? address : ""); pthread_mutex_unlock(&lock);
    fprintf(stderr, "[coop] browser: %s\n", address && *address ? address : "every island");
}
const char *coop_browser_selected(void) { static char copy[64]; pthread_mutex_lock(&lock); memcpy(copy, selected, sizeof copy); pthread_mutex_unlock(&lock); return copy; }
static void *refresh_main(void *arg) { (void)arg; LdnNetworkInfo tmp[16]; coop_lan_scan(tmp, 16, 600); return NULL; }
void coop_browser_refresh(void) { pthread_t t; if (!pthread_create(&t, NULL, refresh_main, NULL)) pthread_detach(t); }

/* ---------- station ---------- */
static void *station_main(void *arg) {
    int fd = (int)(intptr_t)arg;
    static uint8_t body[sizeof(LdnNetworkInfo) + 64];
    for (;;) {
        uint8_t type; uint32_t n;
        if (recv_msg(fd, &type, body, sizeof body, &n)) break;
        if (type == MSG_UPDATE && n == sizeof(LdnNetworkInfo)) ldn_host_network_updated((LdnNetworkInfo *)body);
        else if (type == MSG_NODES && n == sizeof(NodeEntry) * COOP_NODES) {
            NodeEntry *e = (NodeEntry *)body; struct sockaddr_in host; socklen_t hl = sizeof host; getpeername(fd, (struct sockaddr *)&host, &hl);
            pthread_mutex_lock(&lock);
            for (int i = 0; i < COOP_NODES; i++) if (i != self_node) routes[i] = (Route){e[i].used, e[i].ip ? e[i].ip : host.sin_addr.s_addr, e[i].offset};
            pthread_mutex_unlock(&lock);
        }
    }
    /* The thread owns the connection: coop_lan_disconnect only shuts it down (it runs under ldn_host.c's lock, which
       this thread may be waiting for, so it never joins). */
    pthread_mutex_lock(&lock); int expected = !station_running || station_fd != fd;
    if (!expected) { station_running = 0; station_fd = -1; set_self(-1); }
    pthread_mutex_unlock(&lock);
    close(fd);
    if (!expected) { set_status("Local play: the host closed the session"); ldn_host_lost_host(); }
    return NULL;
}
static int lookup_host(const LdnNetworkInfo *target, struct sockaddr_in *out) {
    pthread_mutex_lock(&lock); int found = 0;
    for (int i = 0; i < nislands && !found; i++)
        if (islands[i].seen_ns && !memcmp(&island_info[i].network_id.session, &target->network_id.session, 16)) found = parse_address(islands[i].address, out);
    pthread_mutex_unlock(&lock);
    return found;
}
int coop_lan_connect(const LdnNetworkInfo *target, const char *user_name, int16_t version, LdnNetworkInfo *joined, int *node) {
    struct sockaddr_in host;
    if (!lookup_host(target, &host)) return 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1; if (fd < 0) return 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    set_timeout(fd, 5000);
    char where[32]; format_address(&host, where, sizeof where);
    if (connect(fd, (struct sockaddr *)&host, sizeof host)) { fprintf(stderr, "[coop] cannot reach %s: %s\n", where, strerror(errno)); close(fd); return 2; }
    JoinBody j; memset(&j, 0, sizeof j); snprintf(j.user, sizeof j.user, "%s", user_name); j.version = version; j.offset = coop_port_offset();
    static uint8_t body[sizeof(AcceptBody) + 64]; uint8_t type; uint32_t n;
    if (send_msg(fd, MSG_JOIN, &j, sizeof j) || recv_msg(fd, &type, body, sizeof body, &n)) { close(fd); return 1; }
    if (type != MSG_ACCEPT || n != sizeof(AcceptBody)) { fprintf(stderr, "[coop] %s refused the join\n", where); close(fd); return 1; }
    AcceptBody *a = (AcceptBody *)body; *node = a->node; *joined = a->info;
    if (recv_msg(fd, &type, body, sizeof body, &n) || type != MSG_NODES || n != sizeof(NodeEntry) * COOP_NODES) { close(fd); return 1; }
    pthread_mutex_lock(&lock);
    set_self(*node);
    NodeEntry *e = (NodeEntry *)body;
    for (int i = 0; i < COOP_NODES; i++) if (i != *node) routes[i] = (Route){e[i].used, e[i].ip ? e[i].ip : host.sin_addr.s_addr, e[i].offset};
    station_fd = fd; station_running = 1;
    snprintf(status_line, sizeof status_line, "Visiting %s (%s) as node %d", where, joined->ldn.nodes[0].user_name, *node);
    pthread_mutex_unlock(&lock);
    set_timeout(fd, 0); make_pipe(station_wake);
    if (pthread_create(&station_thread, NULL, station_main, (void *)(intptr_t)fd)) runtime_fail("coop station thread");
    pthread_detach(station_thread);
    fprintf(stderr, "[coop] %s\n", coop_status());
    return 0;
}
int coop_lan_send_edit(const void *body, uint32_t n) {   /* station: to the host (game thread) */
    pthread_mutex_lock(&lock);
    int r = station_running && station_fd >= 0 ? send_msg(station_fd, MSG_EDIT, body, n) : -1;
    pthread_mutex_unlock(&lock);
    return r;
}
void coop_lan_disconnect(void) {
    pthread_mutex_lock(&lock); int fd = station_fd, running = station_running; station_fd = -1; station_running = 0; set_self(-1); pthread_mutex_unlock(&lock);
    if (!running) return;
    shutdown(fd, SHUT_RDWR);   /* station_main closes it */
    set_status("Local play: idle (left the island)");
}
