/* nn::socket replaced by name: the game's sockets are host sockets, and the only addresses it can reach are the virtual
   LDN subnet's (coop.h). The SDK's socket client would talk to bsd:u; nothing in main needs that once these are here.
   - Errors: the game reads Linux errno numbers (pia's wrapper at main+0xe061c tests 11 = EAGAIN, 115 = EINPROGRESS,
     100, 108) and fcntl's O_NONBLOCK is 0x800 (main+0xe02d0). Options and MSG_* flags use FreeBSD's numbers, which are
     Darwin's.
   - Addresses: 10.114.0.(n+1):p goes to node n's real address, port p + its port offset; 10.114.0.255 is sent once
     to each other node. Datagrams carry a 6-byte header (magic, sending node, its virtual port) so the receiver
     knows the virtual source even when several copies share one IP (loopback). Anything else is unreachable: the
     game stays off the internet.
   - Blocking calls give the game's virtual core up while they wait (runtime_core_away). */
#include "runtime.h"
#include "coop.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FD 4096
#define HDR 6
#define SWITCH_O_NONBLOCK 0x800
#define SWITCH_SOCK_NONBLOCK 0x20000000
#define SWITCH_SOCK_CLOEXEC 0x10000000
#define SWITCH_SO_NONBLOCK 0x1100

typedef struct { uint8_t used, dgram, nonblock, has_peer; uint16_t vport; uint32_t peer_ip; uint16_t peer_port; } Sock;
static Sock socks[MAX_FD];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int last_errno;
static int trace = -1;

static int linux_errno(int e) {
    switch (e) {
    case EAGAIN: return 11; case EINPROGRESS: return 115; case EALREADY: return 114; case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89; case EMSGSIZE: return 90; case EPROTOTYPE: return 91; case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93; case EOPNOTSUPP: return 95; case EAFNOSUPPORT: return 97; case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99; case ENETDOWN: return 100; case ENETUNREACH: return 101; case ENETRESET: return 102;
    case ECONNABORTED: return 103; case ECONNRESET: return 104; case ENOBUFS: return 105; case EISCONN: return 106;
    case ENOTCONN: return 107; case ESHUTDOWN: return 108; case ETIMEDOUT: return 110; case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112; case EHOSTUNREACH: return 113;
    default: return e < 35 ? e : 22;   /* 1-34 are the same; anything else EINVAL */
    }
}
static int fail(int e) { last_errno = linux_errno(e); return -1; }
static Sock *sock_of(int fd) { return fd >= 0 && fd < MAX_FD && socks[fd].used ? &socks[fd] : NULL; }
static void log_call(const char *what, int fd, int r) {
    if (trace < 0) trace = getenv("ACNH_COOP_TRACE") != NULL;
    if (trace) fprintf(stderr, "[socket] %s fd=%d -> %d (errno %d)\n", what, fd, r, r < 0 ? last_errno : 0);
}

/* ---------- virtual <-> real addresses ---------- */
static int virtual_node(uint32_t vip) {   /* host order */
    if ((vip & COOP_MASK) != COOP_SUBNET) return -2;
    int n = (int)(vip & 0xff) - 1; return n >= 0 && n < COOP_NODES ? n : (vip == COOP_BROADCAST ? -1 : -2);
}
static int real_of(int node, uint16_t vport, struct sockaddr_in *out) {
    uint32_t ip; int off;
    if (!coop_lan_node_route(node, &ip, &off)) return 0;
    memset(out, 0, sizeof *out); out->sin_len = sizeof *out; out->sin_family = AF_INET;
    out->sin_addr.s_addr = ip; out->sin_port = htons((uint16_t)(vport + off));
    return 1;
}
static void virtual_sockaddr(void *addr, uint32_t *len, uint32_t vip, uint16_t vport) {
    if (!addr || !len) return;
    struct sockaddr_in v = {0}; v.sin_len = sizeof v; v.sin_family = AF_INET; v.sin_addr.s_addr = htonl(vip); v.sin_port = htons(vport);
    uint32_t n = *len < sizeof v ? *len : sizeof v; memcpy(addr, &v, n); *len = sizeof v;
}

/* ---------- lifetime ---------- */
static uint32_t initialize(void *pool, size_t size, size_t alloc, int concurrency) {
    (void)pool; (void)size; (void)alloc; (void)concurrency;
    fprintf(stderr, "[socket] nn::socket initialised on host sockets (port offset %d)\n", coop_port_offset()); return 0;
}
static uint32_t finalize(void) { return 0; }
static int socket_(int domain, int type, int protocol) {
    int base = type & ~(SWITCH_SOCK_NONBLOCK | SWITCH_SOCK_CLOEXEC);
    if (domain != AF_INET || (base != SOCK_DGRAM && base != SOCK_STREAM)) return fail(EAFNOSUPPORT);
    int fd = socket(AF_INET, base, protocol == 17 || protocol == 6 ? protocol : 0);
    if (fd < 0) return fail(errno);
    if (fd >= MAX_FD) { close(fd); return fail(EMFILE); }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (type & SWITCH_SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    pthread_mutex_lock(&lock); socks[fd] = (Sock){.used = 1, .dgram = base == SOCK_DGRAM, .nonblock = !!(type & SWITCH_SOCK_NONBLOCK)}; pthread_mutex_unlock(&lock);
    log_call(base == SOCK_DGRAM ? "socket(udp)" : "socket(tcp)", fd, fd);
    return fd;
}
static int close_(int fd) {
    pthread_mutex_lock(&lock); if (fd >= 0 && fd < MAX_FD) socks[fd].used = 0; pthread_mutex_unlock(&lock);
    return close(fd) ? fail(errno) : 0;
}
static int shutdown_(int fd, int how) { return shutdown(fd, how) ? fail(errno) : 0; }

/* ---------- addresses ---------- */
static int bind_(int fd, const struct sockaddr_in *addr, uint32_t len) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    if (!addr || len < sizeof *addr) return fail(EINVAL);
    uint32_t vip = ntohl(addr->sin_addr.s_addr);
    if (vip != INADDR_ANY && virtual_node(vip) < 0 && vip != INADDR_LOOPBACK) return fail(EADDRNOTAVAIL);
    int off = coop_port_offset(); uint16_t vport = ntohs(addr->sin_port);
    struct sockaddr_in real = {0}; real.sin_len = sizeof real; real.sin_family = AF_INET; real.sin_addr.s_addr = htonl(INADDR_ANY);
    int r = -1;
    if (vport) { real.sin_port = htons((uint16_t)(vport + off)); r = bind(fd, (struct sockaddr *)&real, sizeof real); }
    else for (int tries = 0; tries < 64 && r; tries++) {   /* an ephemeral port: one free at vport + offset */
        vport = (uint16_t)(40000 + arc4random_uniform(9000)); real.sin_port = htons((uint16_t)(vport + off));
        r = bind(fd, (struct sockaddr *)&real, sizeof real);
    }
    if (r) { int e = errno; log_call("bind", fd, -1); return fail(e); }
    pthread_mutex_lock(&lock); s->vport = vport; pthread_mutex_unlock(&lock);
    if (trace) fprintf(stderr, "[socket] bind fd=%d virtual port %u (host %u)\n", fd, vport, vport + off);
    return 0;
}
static int get_sock_name(int fd, void *addr, uint32_t *len) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    int self = coop_lan_self_node();
    if (trace) fprintf(stderr, "[socket] getsockname fd=%d -> node %d port %u\n", fd, self, s->vport);
    virtual_sockaddr(addr, len, self >= 0 ? COOP_SUBNET | (uint32_t)(self + 1) : 0, s->vport);
    return 0;
}
static int get_peer_name(int fd, void *addr, uint32_t *len) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    if (!s->has_peer) return fail(ENOTCONN);
    virtual_sockaddr(addr, len, s->peer_ip, s->peer_port); return 0;
}
static int connect_(int fd, const struct sockaddr_in *addr, uint32_t len) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    if (!addr || len < sizeof *addr) return fail(EINVAL);
    uint32_t vip = ntohl(addr->sin_addr.s_addr); uint16_t vport = ntohs(addr->sin_port);
    int node = virtual_node(vip); struct sockaddr_in real;
    if (node < 0 || !real_of(node, vport, &real)) return fail(ENETUNREACH);
    pthread_mutex_lock(&lock); s->has_peer = 1; s->peer_ip = vip; s->peer_port = vport; pthread_mutex_unlock(&lock);
    if (s->dgram) return 0;   /* datagrams carry their header: sent with sendto to the peer */
    int r; { int away = s->nonblock ? 0 : runtime_core_away(); r = connect(fd, (struct sockaddr *)&real, sizeof real); int e = errno; if (!s->nonblock) runtime_core_back(&away); errno = e; }
    return r ? fail(errno) : 0;
}
static int listen_(int fd, int backlog) { return listen(fd, backlog) ? fail(errno) : 0; }
static int accept_(int fd, void *addr, uint32_t *len) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    struct sockaddr_in from; socklen_t n = sizeof from; int c;
    { int away = s->nonblock ? 0 : runtime_core_away(); c = accept(fd, (struct sockaddr *)&from, &n); int e = errno; if (!s->nonblock) runtime_core_back(&away); errno = e; }
    if (c < 0) return fail(errno);
    if (c >= MAX_FD) { close(c); return fail(EMFILE); }
    int peer = -1;   /* TCP has no header: the node whose real address matches */
    for (int i = 0; i < COOP_NODES && peer < 0; i++) { uint32_t ip; int off; if (coop_lan_node_route(i, &ip, &off) && ip == from.sin_addr.s_addr && i != coop_lan_self_node()) peer = i; }
    uint32_t vip = peer >= 0 ? COOP_SUBNET | (uint32_t)(peer + 1) : 0; uint16_t vport = ntohs(from.sin_port);
    int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    pthread_mutex_lock(&lock); socks[c] = (Sock){.used = 1, .has_peer = 1, .peer_ip = vip, .peer_port = vport}; pthread_mutex_unlock(&lock);
    virtual_sockaddr(addr, len, vip, vport);
    return c;
}

/* ---------- data ---------- */
static __thread uint8_t *frame;
static uint8_t *frame_buf(void) { if (!frame && !(frame = malloc(65536 + HDR))) runtime_fail("socket frame buffer"); return frame; }

static long send_to(int fd, const void *buf, size_t n, int flags, const struct sockaddr_in *addr, uint32_t alen) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    if (!s->dgram) return send(fd, buf, n, flags) < 0 ? fail(errno) : (long)n;
    uint32_t vip; uint16_t vport;
    if (addr && alen >= sizeof *addr) { vip = ntohl(addr->sin_addr.s_addr); vport = ntohs(addr->sin_port); }
    else if (s->has_peer) { vip = s->peer_ip; vport = s->peer_port; }
    else { if (trace) fprintf(stderr, "[socket] sendto fd=%d: no address\n", fd); return fail(EDESTADDRREQ); }
    if (n > 65536) return fail(EMSGSIZE);
    int self = coop_lan_self_node(), node = virtual_node(vip);
    if (node == -2 || self < 0) {
        if (trace) fprintf(stderr, "[socket] sendto fd=%d %zu bytes to %u.%u.%u.%u:%u refused (outside the session)\n", fd, n, vip >> 24, (vip >> 16) & 255, (vip >> 8) & 255, vip & 255, vport);
        return fail(ENETUNREACH);
    }
    uint8_t *f = frame_buf(); f[0] = 'A'; f[1] = 'C'; f[2] = (uint8_t)self; f[3] = 0; f[4] = (uint8_t)(s->vport >> 8); f[5] = (uint8_t)s->vport;
    memcpy(f + HDR, buf, n);
    int sent = 0, tried = 0, err = 0;
    for (int i = node < 0 ? 0 : node; i < (node < 0 ? COOP_NODES : node + 1); i++) {
        if (node < 0 && i == self) continue;
        struct sockaddr_in real; if (!real_of(i, vport, &real)) continue;
        tried++;
        if (sendto(fd, f, n + HDR, flags, (struct sockaddr *)&real, sizeof real) >= 0) sent++; else err = errno;
    }
    if (trace) { const uint8_t *b = buf; fprintf(stderr, "[socket] sendto fd=%d %zu bytes to 10.114.0.%u:%u (%d/%d sent) %02x%02x%02x%02x %02x%02x%02x%02x\n", fd, n, vip & 0xff, vport, sent, tried,
                 b[0], n > 1 ? b[1] : 0, n > 2 ? b[2] : 0, n > 3 ? b[3] : 0, n > 4 ? b[4] : 0, n > 5 ? b[5] : 0, n > 6 ? b[6] : 0, n > 7 ? b[7] : 0); }
    if (node >= 0 && !tried) return fail(EHOSTUNREACH);
    if (tried && !sent) return fail(err);
    return (long)n;
}
static long recv_from(int fd, void *buf, size_t n, int flags, void *addr, uint32_t *alen) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    int blocking = !s->nonblock && !(flags & MSG_DONTWAIT);
    if (!s->dgram) {
        long r; { int away = blocking ? runtime_core_away() : 0; r = recv(fd, buf, n, flags); int e = errno; if (blocking) runtime_core_back(&away); errno = e; }
        if (r < 0) return fail(errno);
        if (addr && alen) virtual_sockaddr(addr, alen, s->peer_ip, s->peer_port);
        return r;
    }
    uint8_t *f = frame_buf();
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from; long r;
        { int away = blocking ? runtime_core_away() : 0; r = recvfrom(fd, f, 65536 + HDR, flags & ~MSG_PEEK, (struct sockaddr *)&from, &fl); int e = errno; if (blocking) runtime_core_back(&away); errno = e; }
        if (r < 0) return fail(errno);
        if (r < HDR || f[0] != 'A' || f[1] != 'C' || f[2] >= COOP_NODES) continue;   /* not from a session member */
        size_t k = (size_t)r - HDR < n ? (size_t)r - HDR : n;
        memcpy(buf, f + HDR, k);
        uint32_t vip = COOP_SUBNET | (uint32_t)(f[2] + 1); uint16_t vport = (uint16_t)(f[4] << 8 | f[5]);
        virtual_sockaddr(addr, alen, vip, vport);
        if (trace) { const uint8_t *b = buf; fprintf(stderr, "[socket] recvfrom fd=%d %zu bytes from 10.114.0.%u:%u %02x%02x%02x%02x %02x%02x%02x%02x\n", fd, k, vip & 0xff, vport,
                 b[0], k > 1 ? b[1] : 0, k > 2 ? b[2] : 0, k > 3 ? b[3] : 0, k > 4 ? b[4] : 0, k > 5 ? b[5] : 0, k > 6 ? b[6] : 0, k > 7 ? b[7] : 0); }
        return (long)k;
    }
}
static long send_(int fd, const void *buf, size_t n, int flags) { return send_to(fd, buf, n, flags, NULL, 0); }
static long recv_(int fd, void *buf, size_t n, int flags) { return recv_from(fd, buf, n, flags, NULL, NULL); }
static long send_to_flag(int fd, const void *buf, size_t n, int flags, const struct sockaddr_in *a, uint32_t l) { return send_to(fd, buf, n, flags, a, l); }

static int poll_(struct pollfd *fds, uint64_t n, int timeout) {
    int r;
    { int away = timeout ? runtime_core_away() : 0; r = poll(fds, (nfds_t)n, timeout); int e = errno; if (timeout) runtime_core_back(&away); errno = e; }
    return r < 0 ? fail(errno) : r;
}
static int fcntl_(int fd, int cmd, int arg) {   /* variadic in the SDK: the guest passes arg in w2 */
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    if (cmd == F_GETFL) return 2 | (s->nonblock ? SWITCH_O_NONBLOCK : 0);   /* O_RDWR */
    if (cmd == F_SETFL) {
        int on = !!(arg & SWITCH_O_NONBLOCK); int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) < 0) return fail(errno);
        s->nonblock = (uint8_t)on; return 0;
    }
    return 0;
}
static int set_sock_opt(int fd, int level, int option, const void *value, uint32_t len) {
    Sock *s = sock_of(fd); if (!s) return fail(EBADF);
    if (level == SOL_SOCKET && option == SWITCH_SO_NONBLOCK && value && len >= 4) {
        int on = *(const int *)value != 0; int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
        s->nonblock = (uint8_t)on; return 0;
    }
    if (level != SOL_SOCKET && level != IPPROTO_TCP) return 0;   /* IP options (TTL, multicast): the mapping decides */
    return setsockopt(fd, level, option, value, len) ? fail(errno) : 0;
}
static int get_sock_opt(int fd, int level, int option, void *value, uint32_t *len) {
    if (!sock_of(fd)) return fail(EBADF);
    socklen_t n = len ? *len : 0;
    if (getsockopt(fd, level, option, value, &n)) return fail(errno);
    if (len) *len = n;
    if (level == SOL_SOCKET && option == SO_ERROR && value && n >= 4) *(int *)value = *(int *)value ? linux_errno(*(int *)value) : 0;
    return 0;
}
static int get_last_errno(void) { return last_errno; }
static void set_last_errno(int e) { last_errno = e; }

/* ---------- helpers that touch no network ---------- */
static uint32_t htonl_(uint32_t v) { return htonl(v); }
static uint16_t htons_(uint16_t v) { return htons(v); }
static int inet_aton_(const char *s, struct in_addr *a) { return inet_aton(s, a); }
static char *inet_ntoa_(uint32_t a) { static __thread char b[16]; struct in_addr x = {a}; return (char *)inet_ntop(AF_INET, &x, b, sizeof b); }
static const char *inet_ntop_(int af, const void *src, char *dst, uint32_t n) { return inet_ntop(af == 2 ? AF_INET : af, src, dst, n); }
static int inet_pton_(int af, const char *src, void *dst) { return inet_pton(af == 2 ? AF_INET : af, src, dst); }
static int get_addr_info(const char *node, const char *service, const void *hints, void **res) {
    (void)service; (void)hints; if (res) *res = NULL;
    fprintf(stderr, "[socket] name lookup refused (offline): %s\n", node ? node : "(null)");
    return 8;   /* EAI_NONAME */
}
static void free_addr_info(void *ai) { (void)ai; }
static const char *gai_str_error(int e) { (void)e; return "name lookup is not available"; }
static void *get_host_by_name(const char *name, const void *options) { (void)options; fprintf(stderr, "[socket] name lookup refused (offline): %s\n", name ? name : "(null)"); return NULL; }
static int resolver_option(void) { return -1; }

void *runtime_socket_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn6socket10InitializeEPvmmi", initialize}, {"_ZN2nn6socket8FinalizeEv", finalize},
        {"_ZN2nn6socket6SocketEiii", socket_}, {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", socket_}, {"nnsocketSocket", socket_},
        {"_ZN2nn6socket5CloseEi", close_}, {"nnsocketClose", close_}, {"_ZN2nn6socket8ShutdownEii", shutdown_},
        {"_ZN2nn6socket4BindEiPK8sockaddrj", bind_}, {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", bind_}, {"nnsocketBind", bind_},
        {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj", get_sock_name}, {"nnsocketGetSockName", get_sock_name}, {"nnsocketGetPeerName", get_peer_name},
        {"_ZN2nn6socket7ConnectEiPK8sockaddrj", connect_}, {"nnsocketConnect", connect_},
        {"_ZN2nn6socket6ListenEii", listen_}, {"_ZN2nn6socket6AcceptEiP8sockaddrPj", accept_},
        {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", send_to}, {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", send_to_flag},
        {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", recv_from}, {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", recv_from},
        {"_ZN2nn6socket4SendEiPKvmi", send_}, {"nnsocketSend", send_}, {"_ZN2nn6socket4RecvEiPvmi", recv_}, {"nnsocketRecv", recv_},
        {"_ZN2nn6socket4PollEP6pollfdmi", poll_}, {"nnsocketPoll", poll_},
        {"_ZN2nn6socket5FcntlEiiz", fcntl_}, {"nnsocketFcntl", fcntl_},
        {"_ZN2nn6socket10SetSockOptEiiiPKvj", set_sock_opt}, {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", set_sock_opt}, {"nnsocketSetSockOpt", set_sock_opt},
        {"_ZN2nn6socket10GetSockOptEiiiPvPj", get_sock_opt}, {"nnsocketGetSockOpt", get_sock_opt},
        {"_ZN2nn6socket12GetLastErrnoEv", get_last_errno}, {"_ZN2nn6socket12GetLastErrorEv", get_last_errno}, {"nnsocketGetLastErrno", get_last_errno},
        {"nnsocketSetLastErrno", set_last_errno},
        {"_ZN2nn6socket9InetHtonlEj", htonl_}, {"_ZN2nn6socket9InetNtohlEj", htonl_}, {"nnsocketInetHtonl", htonl_}, {"nnsocketInetNtohl", htonl_},
        {"_ZN2nn6socket9InetHtonsEt", htons_}, {"_ZN2nn6socket9InetNtohsEt", htons_}, {"nnsocketInetHtons", htons_}, {"nnsocketInetNtohs", htons_},
        {"_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE", inet_aton_}, {"_ZN2nn6socket8InetNtoaE7in_addr", inet_ntoa_},
        {"_ZN2nn6socket8InetNtopEiPKvPcj", inet_ntop_}, {"_ZN2nn6socket8InetPtonEiPKcPv", inet_pton_},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", get_addr_info}, {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", get_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo", free_addr_info}, {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", free_addr_info},
        {"_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", gai_str_error}, {"nnsocketGetHostByNameWithOptions", get_host_by_name},
        {"nnsocketResolverGetOption", resolver_option}, {"nnsocketResolverSetOption", resolver_option},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
