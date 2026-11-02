#if !defined(_WIN32) && !defined(_WIN64)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* accept4: a connection that is non-blocking and close-on-exec from birth */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "proven_sys_net.h"
#include <stdatomic.h>

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
/* ============================================================================
 * Winsock
 * ========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <afunix.h>
#include <windows.h>

typedef SOCKET proven_native_socket_t;
typedef int proven_native_len_t;
typedef WSAPOLLFD proven_native_pollfd_t;
#define NATIVE_INVALID INVALID_SOCKET
#define native_close closesocket

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
/* UDP send and receive offload (USO, URO), from the Windows SDK's ws2ipdef.h; mingw-w64's
 * headers have none of them. */
#ifndef IP_ECN
#define IP_ECN 50
#endif
#ifndef IPV6_ECN
#define IPV6_ECN 50
#endif
#ifndef UDP_SEND_MSG_SIZE
#define UDP_SEND_MSG_SIZE 2
#endif
#ifndef UDP_RECV_MAX_COALESCED_SIZE
#define UDP_RECV_MAX_COALESCED_SIZE 3
#endif
#ifndef UDP_COALESCED_INFO
#define UDP_COALESCED_INFO 3
#endif

static INIT_ONCE g_net_once = INIT_ONCE_STATIC_INIT;
static bool g_net_started;

static BOOL CALLBACK net_start_once(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    WSADATA data;
    g_net_started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return TRUE;
}

/* Winsock refuses every call, name resolution included, until WSAStartup has run. Once per
 * process, on first use; never undone - there is no moment at which the library knows the last
 * socket in the process is gone. */
static bool net_ready(void) {
    InitOnceExecuteOnce(&g_net_once, net_start_once, NULL, NULL);
    return g_net_started;
}

static int net_last_error(void) { return WSAGetLastError(); }

static proven_sys_net_result_t net_reason(int e) {
    switch (e) {
        case WSAEWOULDBLOCK:      return PROVEN_SYS_NET_WOULD_BLOCK;
        case WSAEINPROGRESS:
        case WSAEALREADY:         return PROVEN_SYS_NET_IN_PROGRESS;
        case WSAECONNREFUSED:     return PROVEN_SYS_NET_REFUSED;
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENETRESET:
        case WSAESHUTDOWN:        return PROVEN_SYS_NET_RESET;
        case WSAETIMEDOUT:        return PROVEN_SYS_NET_TIMEOUT;
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAENETDOWN:
        case WSAEHOSTDOWN:        return PROVEN_SYS_NET_UNREACHABLE;
        case WSAEADDRINUSE:       return PROVEN_SYS_NET_ADDR_IN_USE;
        case WSAEADDRNOTAVAIL:    return PROVEN_SYS_NET_ADDR_INVALID;
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT:
        case WSAESOCKTNOSUPPORT:
        case WSAEPFNOSUPPORT:     return PROVEN_SYS_NET_UNSUPPORTED;
        case WSAEACCES:           return PROVEN_SYS_NET_DENIED;
        case WSAEMSGSIZE:         return PROVEN_SYS_NET_TOO_BIG;
        case WSAEMFILE:
        case WSAENOBUFS:
        case WSA_NOT_ENOUGH_MEMORY: return PROVEN_SYS_NET_LIMIT;
        default:                  return PROVEN_SYS_NET_ERROR;
    }
}

static bool net_interrupted(int e) { return e == WSAEINTR; }

static proven_native_socket_t native_open(int af, int type) {
    proven_native_socket_t s = WSASocketW(af, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return s;
    u_long on = 1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

/* An accepted socket does not reliably inherit either property. */
static bool native_prepare_accepted(proven_native_socket_t s) {
    u_long on = 1;
    /* Nor anything else of the listener's but one thing: a WSAEventSelect of the listener's (a
     * selector's notice) carries over to it, and is dropped here. */
    (void)WSAEventSelect(s, NULL, 0);
    if (ioctlsocket(s, FIONBIO, &on) != 0) return false;
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return true;
}

static void native_listener_options(proven_native_socket_t s) {
    /* Not SO_REUSEADDR: on Windows that lets another process bind the same port and take the
     * connections. Exclusive use is the option that means what SO_REUSEADDR means elsewhere. */
    BOOL on = TRUE;
    (void)setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
}

static void native_datagram_options(proven_native_socket_t s) {
    /* Without this, a datagram sent to a port nobody listens on makes a LATER receive on this
     * socket fail with a connection reset - an error about some other packet. */
    BOOL off = FALSE;
    DWORD returned = 0;
    (void)WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &returned, NULL, NULL);
}

#define NATIVE_SEND_FLAGS 0
#define NATIVE_SHUT_WR SD_SEND
#define NATIVE_POLL_IN POLLRDNORM
#define NATIVE_POLL_OUT POLLWRNORM

#else
/* ============================================================================
 * POSIX sockets
 * ========================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if defined(__linux__)
#include <netinet/udp.h>
#include <linux/filter.h>
#endif

typedef int proven_native_socket_t;
typedef socklen_t proven_native_len_t;
typedef struct pollfd proven_native_pollfd_t;
#define NATIVE_INVALID (-1)
#define native_close close

static bool net_ready(void) { return true; }
static int net_last_error(void) { return errno; }

static proven_sys_net_result_t net_reason(int e) {
    switch (e) {
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
                             return PROVEN_SYS_NET_WOULD_BLOCK;
        case EINPROGRESS:
        case EALREADY:       return PROVEN_SYS_NET_IN_PROGRESS;
        case ECONNREFUSED:   return PROVEN_SYS_NET_REFUSED;
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:          return PROVEN_SYS_NET_RESET;
        case ETIMEDOUT:      return PROVEN_SYS_NET_TIMEOUT;
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ENETDOWN:
#ifdef EHOSTDOWN
        case EHOSTDOWN:
#endif
                             return PROVEN_SYS_NET_UNREACHABLE;
        case EADDRINUSE:     return PROVEN_SYS_NET_ADDR_IN_USE;
        case EADDRNOTAVAIL:  return PROVEN_SYS_NET_ADDR_INVALID;
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT:
#ifdef ESOCKTNOSUPPORT
        case ESOCKTNOSUPPORT:
#endif
                             return PROVEN_SYS_NET_UNSUPPORTED;
        case EACCES:
        case EPERM:          return PROVEN_SYS_NET_DENIED;
        case ENOENT:
        case ENOTDIR:        return PROVEN_SYS_NET_NOT_FOUND;
        case EMSGSIZE:       return PROVEN_SYS_NET_TOO_BIG;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:         return PROVEN_SYS_NET_LIMIT;
        default:             return PROVEN_SYS_NET_ERROR;
    }
}

static bool net_interrupted(int e) { return e == EINTR; }

/* Linux makes a socket non-blocking and close-on-exec in the call that creates it; elsewhere
 * the two flags are set right after. */
#if !defined(__linux__)
static bool native_set_flags(proven_native_socket_t s) {
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) != 0) return false;
    int fd_flags = fcntl(s, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(s, F_SETFD, fd_flags | FD_CLOEXEC) != 0) return false;
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return true;
}
#endif

static proven_native_socket_t native_open(int af, int type) {
#if defined(__linux__)
    proven_native_socket_t s = socket(af, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return s;
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return s;
#else
    proven_native_socket_t s = socket(af, type, 0);
    if (s < 0) return s;
    if (!native_set_flags(s)) { close(s); return -1; }
    return s;
#endif
}

static bool native_prepare_accepted(proven_native_socket_t s) {
#if defined(__linux__)
    (void)s;
    return true;            /* accept4 already set both flags */
#else
    return native_set_flags(s);
#endif
}

static void native_listener_options(proven_native_socket_t s) {
    /* A restarted server may bind while connections of the previous run are still in TIME_WAIT. */
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
}

static void native_datagram_options(proven_native_socket_t s) { (void)s; }

#ifdef MSG_NOSIGNAL
#define NATIVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define NATIVE_SEND_FLAGS 0   /* SO_NOSIGPIPE was set when the socket was made */
#endif
#define NATIVE_SHUT_WR SHUT_WR
#define NATIVE_POLL_IN POLLIN
#define NATIVE_POLL_OUT POLLOUT

#endif

/* ============================================================================
 * Shared by both
 * ========================================================================== */

static proven_sys_net_result_t net_fail(void) {
    return net_reason(net_last_error());
}

static int native_family(proven_sys_net_family_t family) {
    switch (family) {
        case PROVEN_SYS_NET_FAMILY_IPV4: return AF_INET;
        case PROVEN_SYS_NET_FAMILY_IPV6: return AF_INET6;
        case PROVEN_SYS_NET_FAMILY_UNIX: return AF_UNIX;
        default: return -1;
    }
}

/* Build the native address. Returns its length, or 0 when the address cannot be expressed. */
static proven_native_len_t to_native(const proven_sys_net_addr_t *a, struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (a->family == PROVEN_SYS_NET_FAMILY_IPV4) {
        struct sockaddr_in *in = (struct sockaddr_in *)out;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->ip, 4);
        return (proven_native_len_t)sizeof *in;
    }
    if (a->family == PROVEN_SYS_NET_FAMILY_IPV6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)out;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a->port);
        memcpy(&in6->sin6_addr, a->ip, 16);
        in6->sin6_scope_id = a->scope_id;
        return (proven_native_len_t)sizeof *in6;
    }
    if (a->family == PROVEN_SYS_NET_FAMILY_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)out;
        if (a->path_len == 0 || a->path_len >= sizeof un->sun_path) return 0;
        un->sun_family = AF_UNIX;
        memcpy(un->sun_path, a->path, a->path_len);
        un->sun_path[a->path_len] = '\0';
        return (proven_native_len_t)sizeof *un;
    }
    return 0;
}

static void from_native(const struct sockaddr_storage *in, proven_native_len_t len, proven_sys_net_addr_t *out) {
    memset(out, 0, sizeof *out);
    if (in->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)in;
        out->family = PROVEN_SYS_NET_FAMILY_IPV4;
        out->port = ntohs(v4->sin_port);
        memcpy(out->ip, &v4->sin_addr, 4);
    } else if (in->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)in;
        out->family = PROVEN_SYS_NET_FAMILY_IPV6;
        out->port = ntohs(v6->sin6_port);
        memcpy(out->ip, &v6->sin6_addr, 16);
        out->scope_id = (proven_u32)v6->sin6_scope_id;
    } else if (in->ss_family == AF_UNIX) {
        /* The peer of a Unix-domain connection is usually unnamed: the family is known and the
         * path is empty. The length the OS reported says how much of sun_path is meaningful. */
        const struct sockaddr_un *un = (const struct sockaddr_un *)in;
        out->family = PROVEN_SYS_NET_FAMILY_UNIX;
        proven_size_t head = (proven_size_t)((const char *)un->sun_path - (const char *)un);
        proven_size_t avail = (proven_size_t)len > head ? (proven_size_t)len - head : 0;
        if (avail > sizeof un->sun_path) avail = sizeof un->sun_path;
        proven_size_t n = 0;
        while (n < avail && n < PROVEN_SYS_NET_UNIX_PATH_MAX && un->sun_path[n] != '\0') n++;
        memcpy(out->path, un->sun_path, n);
        out->path[n] = '\0';
        out->path_len = (proven_u8)n;
    }
}

static proven_sys_net_result_t open_bound(const proven_sys_net_addr_t *addr, int type, bool shared, proven_native_socket_t *out) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    int af = native_family(addr->family);
    struct sockaddr_storage ss;
    proven_native_len_t len = to_native(addr, &ss);
    if (af < 0 || len == 0) return PROVEN_SYS_NET_ADDR_INVALID;

    proven_native_socket_t s = native_open(af, type);
    if (s == NATIVE_INVALID) return net_fail();
    if (af == AF_INET6) {
        int on = 1;
        (void)setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&on, sizeof on);
    }
    if (type == SOCK_STREAM && af != AF_UNIX) native_listener_options(s);
    if (type == SOCK_DGRAM) native_datagram_options(s);
#if defined(__linux__) && defined(SO_REUSEPORT)
    if (shared) {
        int on = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0) {
            native_close(s);
            return PROVEN_SYS_NET_UNSUPPORTED;
        }
    }
#else
    if (shared) {
        native_close(s);
        return PROVEN_SYS_NET_UNSUPPORTED;
    }
#endif

    if (bind(s, (const struct sockaddr *)&ss, len) != 0) {
        proven_sys_net_result_t r = net_fail();
#if defined(_WIN32) || defined(_WIN64)
        /* A port another socket holds exclusively is reported as "access denied" here, and
         * Windows has no privileged ports for that to be confused with. */
        if (r == PROVEN_SYS_NET_DENIED && af != AF_UNIX) r = PROVEN_SYS_NET_ADDR_IN_USE;
#endif
        native_close(s);
        return r;
    }
    *out = s;
    return PROVEN_SYS_NET_OK;
}

static proven_sys_net_result_t open_listener(const proven_sys_net_addr_t *addr, int backlog, bool shared,
                                            proven_sys_socket_t *out) {
    proven_native_socket_t s = NATIVE_INVALID;
    proven_sys_net_result_t r = open_bound(addr, SOCK_STREAM, shared, &s);
    if (r != PROVEN_SYS_NET_OK) return r;
    if (listen(s, backlog > 0 ? backlog : SOMAXCONN) != 0) {
        r = net_fail();
        native_close(s);
        return r;
    }
    *out = (proven_sys_socket_t)s;
    return PROVEN_SYS_NET_OK;
}

static proven_sys_net_result_t open_datagram(const proven_sys_net_addr_t *addr, bool shared, proven_sys_socket_t *out) {
    if (addr->family != PROVEN_SYS_NET_FAMILY_IPV4 && addr->family != PROVEN_SYS_NET_FAMILY_IPV6) {
        return PROVEN_SYS_NET_UNSUPPORTED;
    }
    proven_native_socket_t s = NATIVE_INVALID;
    proven_sys_net_result_t r = open_bound(addr, SOCK_DGRAM, shared, &s);
    if (r != PROVEN_SYS_NET_OK) return r;
    *out = (proven_sys_socket_t)s;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_open_listener(const proven_sys_net_addr_t *addr, int backlog,
                                                     proven_sys_socket_t *out) {
    return open_listener(addr, backlog, false, out);
}

proven_sys_net_result_t proven_sys_net_open_datagram(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out) {
    return open_datagram(addr, false, out);
}

proven_sys_net_result_t proven_sys_net_open_shared(const proven_sys_net_addr_t *addr, bool stream, int backlog,
                                                   proven_sys_socket_t *out) {
    return stream ? open_listener(addr, backlog, true, out) : open_datagram(addr, true, out);
}

proven_sys_net_result_t proven_sys_net_udp_steer(proven_sys_socket_t sock, proven_u32 n) {
#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
    if (n < 1 || n > 255) return PROVEN_SYS_NET_ADDR_INVALID;
    /* The socket's index in the group: the destination connection ID's first byte modulo n -
     * byte 6 of a long header (after the type, the version and the ID's length), byte 1 of a
     * short one. Seen from the UDP payload; a datagram too short to load from ends the program
     * with 0, the first socket. */
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x80, 0, 2),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 6),
        BPF_JUMP(BPF_JMP | BPF_JA, 1, 0, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 1),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, n),
        BPF_STMT(BPF_RET | BPF_A, 0),
    };
    struct sock_fprog prog = { .len = (unsigned short)(sizeof f / sizeof f[0]), .filter = f };
    return setsockopt((proven_native_socket_t)sock, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &prog, (socklen_t)sizeof prog) == 0
        ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_UNSUPPORTED;
#else
    (void)sock; (void)n;
    return PROVEN_SYS_NET_UNSUPPORTED;
#endif
}

proven_sys_net_result_t proven_sys_net_connect_start(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    int af = native_family(addr->family);
    struct sockaddr_storage ss;
    proven_native_len_t len = to_native(addr, &ss);
    if (af < 0 || len == 0) return PROVEN_SYS_NET_ADDR_INVALID;

    proven_native_socket_t s = native_open(af, SOCK_STREAM);
    if (s == NATIVE_INVALID) return net_fail();

    for (;;) {
        if (connect(s, (const struct sockaddr *)&ss, len) == 0) {
            *out = (proven_sys_socket_t)s;
            return PROVEN_SYS_NET_OK;
        }
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        proven_sys_net_result_t r = net_reason(e);
        /* A non-blocking connect that has merely started says "would block" on Windows and
         * "in progress" on POSIX. A Unix-domain connect to a full backlog says "would block"
         * on Linux and means the same thing to the caller: not connected, try again later. */
        if (r == PROVEN_SYS_NET_WOULD_BLOCK || r == PROVEN_SYS_NET_IN_PROGRESS) {
            *out = (proven_sys_socket_t)s;
            return PROVEN_SYS_NET_IN_PROGRESS;
        }
        native_close(s);
#if defined(_WIN32) || defined(_WIN64)
        /* Winsock answers a Unix-domain connect to a path that does not exist the way it
         * answers one that nobody listens on. POSIX tells the two apart, and a caller needs to:
         * one means "start the server", the other "the server is not accepting". */
        if (af == AF_UNIX) {
            WCHAR wide[PROVEN_SYS_NET_UNIX_PATH_MAX + 1];
            int n = MultiByteToWideChar(CP_UTF8, 0, addr->path, (int)addr->path_len, wide, PROVEN_SYS_NET_UNIX_PATH_MAX);
            if (n > 0) {
                wide[n] = 0;
                if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) {
                    DWORD fe = GetLastError();
                    if (fe == ERROR_FILE_NOT_FOUND || fe == ERROR_PATH_NOT_FOUND) return PROVEN_SYS_NET_NOT_FOUND;
                }
            }
        }
#endif
        return r;
    }
}

proven_sys_net_result_t proven_sys_net_pair(proven_sys_socket_t *a, proven_sys_socket_t *b) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
#if defined(_WIN32) || defined(_WIN64)
    /* No socketpair here: listen on a loopback port the system chooses, connect to it, and
     * accept. The listener exists for a moment; the accepted socket is checked to be the one
     * that connected, so that another local process racing for that port cannot end up as one
     * end of the pair. */
    proven_native_socket_t listener = native_open(AF_INET, SOCK_STREAM);
    if (listener == NATIVE_INVALID) return net_fail();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    proven_native_len_t len = (proven_native_len_t)sizeof addr;
    proven_native_socket_t client = NATIVE_INVALID, server = NATIVE_INVALID;
    proven_sys_net_result_t r = PROVEN_SYS_NET_ERROR;
    if (bind(listener, (const struct sockaddr *)&addr, len) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) != 0) { r = net_fail(); goto done; }
    client = native_open(AF_INET, SOCK_STREAM);
    if (client == NATIVE_INVALID) { r = net_fail(); goto done; }
    if (connect(client, (const struct sockaddr *)&addr, len) != 0) {
        proven_sys_net_result_t cr = net_fail();
        if (cr != PROVEN_SYS_NET_WOULD_BLOCK && cr != PROVEN_SYS_NET_IN_PROGRESS) { r = cr; goto done; }
    }
    for (int tries = 0; tries < 200 && server == NATIVE_INVALID; ++tries) {
        struct sockaddr_in from;
        proven_native_len_t fl = (proven_native_len_t)sizeof from;
        proven_native_socket_t s = accept(listener, (struct sockaddr *)&from, &fl);
        if (s == NATIVE_INVALID) {
            if (net_reason(net_last_error()) != PROVEN_SYS_NET_WOULD_BLOCK) { r = net_fail(); goto done; }
            Sleep(5);
            continue;
        }
        struct sockaddr_in mine;
        proven_native_len_t ml = (proven_native_len_t)sizeof mine;
        if (getsockname(client, (struct sockaddr *)&mine, &ml) == 0 && mine.sin_port == from.sin_port &&
            from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            server = s;
        } else {
            closesocket(s);                     /* somebody else's connection */
        }
    }
    if (server == NATIVE_INVALID) { r = PROVEN_SYS_NET_TIMEOUT; goto done; }
    if (!native_prepare_accepted(server)) { r = PROVEN_SYS_NET_ERROR; goto done; }
    closesocket(listener);
    *a = (proven_sys_socket_t)client;
    *b = (proven_sys_socket_t)server;
    return PROVEN_SYS_NET_OK;
done:
    if (listener != NATIVE_INVALID) closesocket(listener);
    if (client != NATIVE_INVALID) closesocket(client);
    if (server != NATIVE_INVALID) closesocket(server);
    return r;
#else
    int fds[2];
#if defined(__linux__)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) return net_fail();
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return net_fail();
    if (!native_set_flags(fds[0]) || !native_set_flags(fds[1])) { close(fds[0]); close(fds[1]); return PROVEN_SYS_NET_ERROR; }
#endif
    *a = (proven_sys_socket_t)fds[0];
    *b = (proven_sys_socket_t)fds[1];
    return PROVEN_SYS_NET_OK;
#endif
}

proven_sys_net_result_t proven_sys_net_connect_result(proven_sys_socket_t sock) {
    int err = 0;
    proven_native_len_t len = (proven_native_len_t)sizeof err;
    if (getsockopt((proven_native_socket_t)sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) return net_fail();
    if (err != 0) return net_reason(err);
    /* No error recorded and yet not connected happens when the wait woke for another reason:
     * the peer address is the test that cannot be fooled. */
    struct sockaddr_storage ss;
    proven_native_len_t sl = (proven_native_len_t)sizeof ss;
    if (getpeername((proven_native_socket_t)sock, (struct sockaddr *)&ss, &sl) != 0) return PROVEN_SYS_NET_IN_PROGRESS;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_accept(proven_sys_socket_t listener, proven_sys_socket_t *out,
                                              proven_sys_net_addr_t *peer) {
    for (;;) {
        struct sockaddr_storage ss;
        proven_native_len_t len = (proven_native_len_t)sizeof ss;
#if defined(__linux__)
        proven_native_socket_t s = accept4((proven_native_socket_t)listener, (struct sockaddr *)&ss, &len,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        proven_native_socket_t s = accept((proven_native_socket_t)listener, (struct sockaddr *)&ss, &len);
#endif
        if (s == NATIVE_INVALID) {
            int e = net_last_error();
            if (net_interrupted(e)) continue;
            proven_sys_net_result_t r = net_reason(e);
            /* A connection that was reset while it waited in the backlog is not a failure of
             * the listener: skip it and take the next one. */
            if (r == PROVEN_SYS_NET_RESET) continue;
            return r;
        }
        if (!native_prepare_accepted(s)) {
            native_close(s);
            return PROVEN_SYS_NET_ERROR;
        }
        if (peer) from_native(&ss, len, peer);
        *out = (proven_sys_socket_t)s;
        return PROVEN_SYS_NET_OK;
    }
}

/* One call may not be asked for more than the platform's int-sized length. */
static int clamp_len(proven_size_t n) {
    return n > 0x40000000u ? 0x40000000 : (int)n;
}

/* How much one send may offer. Winsock accepts a buffer of any size in one non-blocking send
 * and queues all of it, whatever SO_SNDBUF says - so a 48 MiB write to a peer that reads
 * nothing "succeeds", and neither a deadline nor back-pressure ever comes into play. Offered in
 * pieces of this size, the socket refuses once its buffer is full, as it does everywhere else. */
static int clamp_send(proven_size_t n) {
#if defined(_WIN32) || defined(_WIN64)
    return n > 65536u ? 65536 : (int)n;
#else
    return clamp_len(n);
#endif
}

proven_sys_net_result_t proven_sys_net_recv(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                            proven_size_t *received) {
    *received = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        int n = recv((proven_native_socket_t)sock, (char *)buf, clamp_len(cap), 0);
#else
        ssize_t n = recv((proven_native_socket_t)sock, buf, (size_t)clamp_len(cap), 0);
#endif
        if (n > 0) { *received = (proven_size_t)n; return PROVEN_SYS_NET_OK; }
        if (n == 0) return PROVEN_SYS_NET_EOF;
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

proven_sys_net_result_t proven_sys_net_send(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                            proven_size_t *sent) {
    *sent = 0;
    if (len == 0) return PROVEN_SYS_NET_OK;
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        int n = send((proven_native_socket_t)sock, (const char *)buf, clamp_send(len), NATIVE_SEND_FLAGS);
#else
        ssize_t n = send((proven_native_socket_t)sock, buf, (size_t)clamp_send(len), NATIVE_SEND_FLAGS);
#endif
        if (n >= 0) { *sent = (proven_size_t)n; return PROVEN_SYS_NET_OK; }
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

/* Several buffers in one call, in order: writev's sendmsg on POSIX (with the same no-SIGPIPE
 * flag as send), WSASend on Windows. Offered up to PROVEN_SYS_NET_VEC_MAX buffers and, as
 * clamp_send says, at most 64 KiB in all on Windows. */
proven_sys_net_result_t proven_sys_net_send_vec(proven_sys_socket_t sock, const proven_sys_net_vec_t *vec, proven_size_t count,
                                                proven_size_t *sent) {
    *sent = 0;
    if (count > PROVEN_SYS_NET_VEC_MAX) count = PROVEN_SYS_NET_VEC_MAX;
    proven_size_t total = 0, used = 0;
#if defined(_WIN32) || defined(_WIN64)
    WSABUF bufs[PROVEN_SYS_NET_VEC_MAX];
    for (; used < count && total < 65536u; ++used) {
        proven_size_t n = vec[used].len;
        if (n > 65536u - total) n = 65536u - total;
        bufs[used].buf = (char *)vec[used].ptr;
        bufs[used].len = (ULONG)n;
        total += n;
    }
    if (total == 0) return PROVEN_SYS_NET_OK;
    for (;;) {
        DWORD n = 0;
        if (WSASend((proven_native_socket_t)sock, bufs, (DWORD)used, &n, 0, NULL, NULL) == 0) { *sent = (proven_size_t)n; return PROVEN_SYS_NET_OK; }
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
#else
    struct iovec iov[PROVEN_SYS_NET_VEC_MAX];
    for (; used < count && total < 0x40000000u; ++used) {
        proven_size_t n = vec[used].len;
        if (n > 0x40000000u - total) n = 0x40000000u - total;
        iov[used].iov_base = (void *)vec[used].ptr;
        iov[used].iov_len = (size_t)n;
        total += n;
    }
    if (total == 0) return PROVEN_SYS_NET_OK;
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = iov;
    msg.msg_iovlen = (typeof(msg.msg_iovlen))used;
    for (;;) {
        ssize_t n = sendmsg((proven_native_socket_t)sock, &msg, NATIVE_SEND_FLAGS);
        if (n >= 0) { *sent = (proven_size_t)n; return PROVEN_SYS_NET_OK; }
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
#endif
}

proven_sys_net_result_t proven_sys_net_recv_from(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                                 proven_size_t *received, proven_sys_net_addr_t *from) {
    *received = 0;
    for (;;) {
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
#if defined(_WIN32) || defined(_WIN64)
        proven_native_len_t sl = (proven_native_len_t)sizeof ss;
        int n = recvfrom((proven_native_socket_t)sock, (char *)buf, clamp_len(cap), 0, (struct sockaddr *)&ss, &sl);
        if (n >= 0) {
            *received = (proven_size_t)n;
            if (from) from_native(&ss, sl, from);
            return PROVEN_SYS_NET_OK;
        }
        int e = net_last_error();
        if (e == WSAEMSGSIZE) {
            /* The buffer was filled and the rest of the datagram discarded. */
            *received = (proven_size_t)clamp_len(cap);
            if (from) from_native(&ss, sl, from);
            return PROVEN_SYS_NET_TRUNCATED;
        }
#else
        /* recvmsg, not recvfrom: only msg_flags says the datagram was cut. */
        struct iovec iov = { .iov_base = buf, .iov_len = (size_t)clamp_len(cap) };
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_name = &ss;
        msg.msg_namelen = (socklen_t)sizeof ss;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t n = recvmsg((proven_native_socket_t)sock, &msg, 0);
        if (n >= 0) {
            *received = (proven_size_t)n;
            if (from) from_native(&ss, msg.msg_namelen, from);
            return (msg.msg_flags & MSG_TRUNC) ? PROVEN_SYS_NET_TRUNCATED : PROVEN_SYS_NET_OK;
        }
        int e = net_last_error();
#endif
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

proven_sys_net_result_t proven_sys_net_send_to(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                               const proven_sys_net_addr_t *to) {
    struct sockaddr_storage ss;
    proven_native_len_t sl = to_native(to, &ss);
    if (sl == 0) return PROVEN_SYS_NET_ADDR_INVALID;
    if (len > 0x40000000u) return PROVEN_SYS_NET_TOO_BIG;
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        int n = sendto((proven_native_socket_t)sock, (const char *)buf, (int)len, NATIVE_SEND_FLAGS,
                       (const struct sockaddr *)&ss, sl);
#else
        ssize_t n = sendto((proven_native_socket_t)sock, buf, len, NATIVE_SEND_FLAGS,
                           (const struct sockaddr *)&ss, sl);
#endif
        if (n >= 0) return (proven_size_t)n == len ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_ERROR;
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

/* RFC-0018 S-3: batched datagrams. Any thread may send first, so the process-wide flags are
 * atomic (relaxed: each is a hint that a later call reads again). */
static atomic_bool g_segments_off;      /* the test switch */
#if defined(__linux__) && defined(UDP_SEGMENT)
static atomic_bool g_gso_refused;       /* this kernel has no GSO for UDP: not asked again */
static atomic_bool g_gso_probed;        /* the option was asked for once */
static atomic_bool g_gso_worked;        /* a GSO send has succeeded in this process */

/* A kernel before 4.18 has no UDP_SEGMENT and skips the control message without an error: every
 * batch would leave as one large datagram. Asking for the socket option (0: no default size)
 * tells such a kernel apart - ENOPROTOOPT - before the first batch. */
static void gso_probe(proven_sys_socket_t sock) {
    if (atomic_load_explicit(&g_gso_probed, memory_order_relaxed)) return;
    const int zero = 0;
    if (setsockopt((proven_native_socket_t)sock, IPPROTO_UDP, UDP_SEGMENT, &zero, (socklen_t)sizeof zero) != 0 && errno == ENOPROTOOPT)
        atomic_store_explicit(&g_gso_refused, true, memory_order_relaxed);
    atomic_store_explicit(&g_gso_probed, true, memory_order_relaxed);
}
#elif defined(__linux__)
static atomic_bool g_gso_refused, g_gso_worked;   /* headers without UDP_SEGMENT: never set */
#endif

#if defined(_WIN32) || defined(_WIN64)
static atomic_bool g_gso_refused;       /* this Windows has no USO: not asked again */
static atomic_bool g_gso_probed;
static atomic_bool g_gso_worked;

/* A Windows without USO might pass over the control message and send the batch as one large
 * datagram, as a Linux before 4.18 would: the socket option, asked for once (0: no default
 * size), tells it apart first. */
static void gso_probe(proven_sys_socket_t sock) {
    if (atomic_load_explicit(&g_gso_probed, memory_order_relaxed)) return;
    const DWORD zero = 0;
    if (setsockopt((proven_native_socket_t)sock, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (const char *)&zero, (int)sizeof zero) != 0)
        atomic_store_explicit(&g_gso_refused, true, memory_order_relaxed);
    atomic_store_explicit(&g_gso_probed, true, memory_order_relaxed);
}

/* WSARecvMsg is reached through a pointer the provider hands out; fetched once, with the first
 * socket that asks. */
static INIT_ONCE g_recvmsg_once = INIT_ONCE_STATIC_INIT;
static LPFN_WSARECVMSG g_recvmsg;

static BOOL CALLBACK recvmsg_find(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)context;
    GUID id = WSAID_WSARECVMSG;
    DWORD n = 0;
    if (WSAIoctl(*(const proven_native_socket_t *)param, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, (DWORD)sizeof id, &g_recvmsg, (DWORD)sizeof g_recvmsg, &n, NULL, NULL) != 0)
        g_recvmsg = NULL;
    return TRUE;
}
#endif

void proven_sys_net_segments_off_for_test_(bool off) { atomic_store_explicit(&g_segments_off, off, memory_order_relaxed); }

#if defined(__linux__) || defined(_WIN32) || defined(_WIN64)
/* ECN on sends (RFC-0035): a control message per send. A system that refuses it - before it has
 * ever taken one - is not asked again in this process, and what was to go marked goes unmarked:
 * the connection's ECN validation then fails on its own. */
static atomic_bool g_ecn_refused;
static atomic_bool g_ecn_worked;

static bool ecn_refusal(bool refused) {
    if (!refused || atomic_load_explicit(&g_ecn_worked, memory_order_relaxed)) return false;
    atomic_store_explicit(&g_ecn_refused, true, memory_order_relaxed);
    return true;
}

#if defined(__linux__)
/* One sendmsg of `n` bytes: datagrams of `gso` bytes when not 0 (UDP_SEGMENT), with the ECN field
 * `ecn` when not 0. 0 when sent (`*sent` bytes), else the error. */
static int msg_send(proven_sys_socket_t sock, const unsigned char *p, proven_size_t n, proven_size_t gso, const struct sockaddr_storage *ss,
                    proven_native_len_t sl, proven_sys_net_family_t family, proven_u8 ecn, proven_size_t *sent) {
    struct iovec iov = { .iov_base = (void *)(proven_uintptr_t)p, .iov_len = (size_t)n };
    union { char b[CMSG_SPACE(sizeof(proven_u16)) + CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = (void *)(proven_uintptr_t)ss;
    msg.msg_namelen = sl;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.b;
    msg.msg_controllen = sizeof ctl.b;
    size_t used = 0;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
#if defined(UDP_SEGMENT)
    if (gso) {
        c->cmsg_level = IPPROTO_UDP;
        c->cmsg_type = UDP_SEGMENT;
        c->cmsg_len = CMSG_LEN(sizeof(proven_u16));
        const proven_u16 size = (proven_u16)gso;
        memcpy(CMSG_DATA(c), &size, sizeof size);
        used += CMSG_SPACE(sizeof(proven_u16));
        c = CMSG_NXTHDR(&msg, c);
    }
#else
    (void)gso;
#endif
    if (ecn) {
        const int tos = ecn & 3;
        c->cmsg_level = family == PROVEN_SYS_NET_FAMILY_IPV6 ? IPPROTO_IPV6 : IPPROTO_IP;
        c->cmsg_type = family == PROVEN_SYS_NET_FAMILY_IPV6 ? IPV6_TCLASS : IP_TOS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &tos, sizeof tos);
        used += CMSG_SPACE(sizeof(int));
    }
    msg.msg_controllen = used;
    if (used == 0) msg.msg_control = NULL;
    for (;;) {
        const ssize_t k = sendmsg((proven_native_socket_t)sock, &msg, NATIVE_SEND_FLAGS);
        if (k >= 0) { *sent = (proven_size_t)k; return 0; }
        const int e = net_last_error();
        if (!net_interrupted(e)) return e;
    }
}

static bool msg_would_block(int e) {
    return e == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        || e == EWOULDBLOCK
#endif
        ;
}
#define MSG_REFUSED(e) ((e) == EINVAL || (e) == ENOPROTOOPT || (e) == EOPNOTSUPP)
#elif defined(_WIN32) || defined(_WIN64)
static int msg_send(proven_sys_socket_t sock, const unsigned char *p, proven_size_t n, proven_size_t gso, const struct sockaddr_storage *ss,
                    proven_native_len_t sl, proven_sys_net_family_t family, proven_u8 ecn, proven_size_t *sent) {
    WSABUF b = { .len = (ULONG)n, .buf = (CHAR *)(proven_uintptr_t)p };
    union { char b[WSA_CMSG_SPACE(sizeof(DWORD)) + WSA_CMSG_SPACE(sizeof(INT))]; WSACMSGHDR align; } ctl;
    memset(&ctl, 0, sizeof ctl);
    WSAMSG msg;
    memset(&msg, 0, sizeof msg);
    msg.name = (LPSOCKADDR)(proven_uintptr_t)ss;
    msg.namelen = sl;
    msg.lpBuffers = &b;
    msg.dwBufferCount = 1;
    msg.Control.buf = ctl.b;
    msg.Control.len = (ULONG)sizeof ctl.b;
    ULONG used = 0;
    WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&msg);
    if (gso) {
        c->cmsg_level = IPPROTO_UDP;
        c->cmsg_type = UDP_SEND_MSG_SIZE;
        c->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
        const DWORD size = (DWORD)gso;
        memcpy(WSA_CMSG_DATA(c), &size, sizeof size);
        used += (ULONG)WSA_CMSG_SPACE(sizeof(DWORD));
        c = WSA_CMSG_NXTHDR(&msg, c);
    }
    if (ecn) {
        const INT mark = ecn & 3;
        c->cmsg_level = family == PROVEN_SYS_NET_FAMILY_IPV6 ? IPPROTO_IPV6 : IPPROTO_IP;
        c->cmsg_type = family == PROVEN_SYS_NET_FAMILY_IPV6 ? IPV6_ECN : IP_ECN;
        c->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
        memcpy(WSA_CMSG_DATA(c), &mark, sizeof mark);
        used += (ULONG)WSA_CMSG_SPACE(sizeof(INT));
    }
    msg.Control.len = used;
    if (used == 0) msg.Control.buf = NULL;
    DWORD k = 0;
    if (WSASendMsg((proven_native_socket_t)sock, &msg, 0, &k, NULL, NULL) == 0) { *sent = (proven_size_t)k; return 0; }
    return net_last_error();
}

static bool msg_would_block(int e) { return e == WSAEWOULDBLOCK; }
#define MSG_REFUSED(e) ((e) == WSAEINVAL || (e) == WSAENOPROTOOPT || (e) == WSAEOPNOTSUPP)
#endif
#endif

proven_sys_net_result_t proven_sys_net_send_segments(proven_sys_socket_t sock, const void *buf, proven_size_t total,
                                                     proven_size_t seg, const proven_sys_net_addr_t *to, proven_u8 ecn, proven_size_t *sent) {
    const unsigned char *p = (const unsigned char *)buf;
    *sent = 0;
    if (total == 0) return PROVEN_SYS_NET_OK;
    if (seg == 0 || total > PROVEN_SYS_NET_SEGMENTS_MAX_BYTES || (total + seg - 1) / seg > PROVEN_SYS_NET_SEGMENTS_MAX_COUNT) return PROVEN_SYS_NET_TOO_BIG;
#if defined(__linux__) || defined(_WIN32) || defined(_WIN64)
    proven_u8 mark = atomic_load_explicit(&g_ecn_refused, memory_order_relaxed) ? 0 : (proven_u8)(ecn & 3);
    bool gso = false;
#if (defined(__linux__) && defined(UDP_SEGMENT)) || defined(_WIN32) || defined(_WIN64)
    if (total > seg && !atomic_load_explicit(&g_segments_off, memory_order_relaxed)) {
        gso_probe(sock);
        gso = !atomic_load_explicit(&g_gso_refused, memory_order_relaxed);
    }
#endif
    if (gso || mark) {
        struct sockaddr_storage ss;
        const proven_native_len_t sl = to_native(to, &ss);
        if (sl == 0) return PROVEN_SYS_NET_ADDR_INVALID;
        /* GSO (Linux) or USO (Windows): one call. */
        while (gso) {
            proven_size_t n = 0;
            const int e = msg_send(sock, p, total, seg, &ss, sl, to->family, mark, &n);
            if (e == 0) {
                atomic_store_explicit(&g_gso_worked, true, memory_order_relaxed);
                if (mark) atomic_store_explicit(&g_ecn_worked, true, memory_order_relaxed);
                *sent = n;
                return n == total ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_ERROR;
            }
            if (msg_would_block(e)) return PROVEN_SYS_NET_WOULD_BLOCK;
            if (mark && ecn_refusal(MSG_REFUSED(e))) { mark = 0; continue; }   /* the mark, not the batch: again without it */
            /* Any other failure: this batch goes one datagram at a time - not dropped, which would
             * read as a path that lost them all. Remembered for the process only while GSO has
             * never worked here: afterwards the same errors (EINVAL when a route's MTU has
             * shrunk below the segment size, EMSGSIZE) belong to this batch, not to the kernel. */
            if (MSG_REFUSED(e) && !atomic_load_explicit(&g_gso_worked, memory_order_relaxed))
                atomic_store_explicit(&g_gso_refused, true, memory_order_relaxed);
            break;
        }
        /* One datagram at a time, marked. */
        while (mark && *sent < total) {
            const proven_size_t n = total - *sent < seg ? total - *sent : seg;
            proven_size_t k = 0;
            const int e = msg_send(sock, p + *sent, n, 0, &ss, sl, to->family, mark, &k);
            if (e == 0) {
                atomic_store_explicit(&g_ecn_worked, true, memory_order_relaxed);
                if (k != n) return PROVEN_SYS_NET_ERROR;
                *sent += n;
                continue;
            }
            if (msg_would_block(e)) return PROVEN_SYS_NET_WOULD_BLOCK;
            if (ecn_refusal(MSG_REFUSED(e))) { mark = 0; break; }
            return net_reason(e);
        }
    }
#else
    (void)ecn;
#endif
    while (*sent < total) {
        const proven_size_t n = total - *sent < seg ? total - *sent : seg;
        const proven_sys_net_result_t r = proven_sys_net_send_to(sock, p + *sent, n, to);
        if (r != PROVEN_SYS_NET_OK) return r;
        *sent += n;
    }
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_udp_ecn(proven_sys_socket_t sock, proven_sys_net_family_t family) {
    const bool v6 = family == PROVEN_SYS_NET_FAMILY_IPV6;
#if defined(__linux__) && defined(IP_RECVTOS) && defined(IPV6_RECVTCLASS)
    const int on = 1;
    return setsockopt((proven_native_socket_t)sock, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_RECVTCLASS : IP_RECVTOS, &on, (socklen_t)sizeof on) == 0
        ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_UNSUPPORTED;
#elif defined(_WIN32) || defined(_WIN64)
    /* Read through WSARecvMsg's control data only. */
    proven_native_socket_t ns = (proven_native_socket_t)sock;
    InitOnceExecuteOnce(&g_recvmsg_once, recvmsg_find, &ns, NULL);
    if (!g_recvmsg) return PROVEN_SYS_NET_UNSUPPORTED;
    const DWORD on = 1;
    return setsockopt(ns, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_ECN : IP_ECN, (const char *)&on, (int)sizeof on) == 0
        ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_UNSUPPORTED;
#else
    (void)sock; (void)v6;
    return PROVEN_SYS_NET_UNSUPPORTED;
#endif
}

#if defined(__linux__)
/* The ECN field in a received control message (IP_TOS: one byte; IPV6_TCLASS: an int), or -1
 * when the message is not one. */
static int ecn_of(const struct cmsghdr *c) {
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS && c->cmsg_len >= CMSG_LEN(1)) return *(const unsigned char *)CMSG_DATA(c) & 3;
    if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
        int v = 0;
        memcpy(&v, CMSG_DATA(c), sizeof v);
        return v & 3;
    }
    return -1;
}
#elif defined(_WIN32) || defined(_WIN64)
static int ecn_of(const WSACMSGHDR *c) {
    const bool ip = c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_ECN || c->cmsg_type == IP_TOS);
    const bool ip6 = c->cmsg_level == IPPROTO_IPV6 && (c->cmsg_type == IPV6_ECN || c->cmsg_type == IPV6_TCLASS);
    if ((!ip && !ip6) || c->cmsg_len < WSA_CMSG_LEN(sizeof(INT))) return -1;
    INT v = 0;
    memcpy(&v, WSA_CMSG_DATA(c), sizeof v);
    return v & 3;
}
#endif

proven_sys_net_result_t proven_sys_net_udp_gro(proven_sys_socket_t sock) {
#if defined(__linux__) && defined(UDP_GRO)
    if (atomic_load_explicit(&g_segments_off, memory_order_relaxed)) return PROVEN_SYS_NET_UNSUPPORTED;
    const int on = 1;
    return setsockopt((proven_native_socket_t)sock, IPPROTO_UDP, UDP_GRO, &on, (socklen_t)sizeof on) == 0 ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_UNSUPPORTED;
#elif defined(_WIN32) || defined(_WIN64)
    /* URO, coalescing no more than a batch, so that a receive always fits the drivers' buffer. */
    if (atomic_load_explicit(&g_segments_off, memory_order_relaxed)) return PROVEN_SYS_NET_UNSUPPORTED;
    /* Only with WSARecvMsg at hand: without it the size of a coalesced receive could not be read. */
    proven_native_socket_t ns = (proven_native_socket_t)sock;
    InitOnceExecuteOnce(&g_recvmsg_once, recvmsg_find, &ns, NULL);
    if (!g_recvmsg) return PROVEN_SYS_NET_UNSUPPORTED;
    const DWORD most = (DWORD)PROVEN_SYS_NET_SEGMENTS_MAX_BYTES;
    return setsockopt((proven_native_socket_t)sock, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, (const char *)&most, (int)sizeof most) == 0 ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_UNSUPPORTED;
#else
    (void)sock;
    return PROVEN_SYS_NET_UNSUPPORTED;
#endif
}

proven_sys_net_result_t proven_sys_net_udp_dontfrag(proven_sys_socket_t sock, proven_sys_net_family_t family) {
    int level = -1, name = -1, value = 1;
    if (family == PROVEN_SYS_NET_FAMILY_IPV4) {
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_PROBE)
        level = IPPROTO_IP; name = IP_MTU_DISCOVER; value = IP_PMTUDISC_PROBE;
#elif defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
        level = IPPROTO_IP; name = IP_MTU_DISCOVER; value = IP_PMTUDISC_DO;
#elif defined(IP_DONTFRAGMENT)
        level = IPPROTO_IP; name = IP_DONTFRAGMENT;
#elif defined(IP_DONTFRAG)
        level = IPPROTO_IP; name = IP_DONTFRAG;
#endif
    } else if (family == PROVEN_SYS_NET_FAMILY_IPV6) {
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_PROBE)
        level = IPPROTO_IPV6; name = IPV6_MTU_DISCOVER; value = IPV6_PMTUDISC_PROBE;
#elif defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DO)
        level = IPPROTO_IPV6; name = IPV6_MTU_DISCOVER; value = IPV6_PMTUDISC_DO;
#elif defined(IPV6_DONTFRAG)
        level = IPPROTO_IPV6; name = IPV6_DONTFRAG;
#endif
    }
    if (level < 0) return PROVEN_SYS_NET_UNSUPPORTED;
    return setsockopt((proven_native_socket_t)sock, level, name, (const char *)&value, (proven_native_len_t)sizeof value) == 0 ? PROVEN_SYS_NET_OK : net_fail();
}

proven_sys_net_result_t proven_sys_net_recv_segments(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                                     proven_size_t *received, proven_size_t *seg, proven_u8 *ecn, proven_sys_net_addr_t *from) {
    *seg = 0;
    if (ecn) *ecn = 0;
#if defined(__linux__)
    *received = 0;
    for (;;) {
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        struct iovec iov = { .iov_base = buf, .iov_len = (size_t)clamp_len(cap) };
        union { char b[CMSG_SPACE(sizeof(int)) * 4]; struct cmsghdr align; } ctl;
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_name = &ss;
        msg.msg_namelen = (socklen_t)sizeof ss;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctl.b;
        msg.msg_controllen = sizeof ctl.b;
        const ssize_t n = recvmsg((proven_native_socket_t)sock, &msg, 0);
        if (n >= 0) {
            *received = (proven_size_t)n;
            if (from) from_native(&ss, msg.msg_namelen, from);
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                const int mark = ecn_of(c);
                if (mark >= 0) { if (ecn) *ecn = (proven_u8)mark; continue; }
#if defined(UDP_GRO)
                if (c->cmsg_level != IPPROTO_UDP || c->cmsg_type != UDP_GRO) continue;
                int size = 0;
                memcpy(&size, CMSG_DATA(c), sizeof size);
                if (size > 0 && (proven_size_t)size < *received) *seg = (proven_size_t)size;
#endif
            }
            return (msg.msg_flags & MSG_TRUNC) ? PROVEN_SYS_NET_TRUNCATED : PROVEN_SYS_NET_OK;
        }
        const int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
#elif defined(_WIN32) || defined(_WIN64)
    /* URO: WSARecvMsg, for the segment size in UDP_COALESCED_INFO. */
    proven_native_socket_t ns = (proven_native_socket_t)sock;
    InitOnceExecuteOnce(&g_recvmsg_once, recvmsg_find, &ns, NULL);
    if (!g_recvmsg || atomic_load_explicit(&g_segments_off, memory_order_relaxed)) return proven_sys_net_recv_from(sock, buf, cap, received, from);
    *received = 0;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    WSABUF b = { .len = (ULONG)clamp_len(cap), .buf = (CHAR *)buf };
    union { char b[WSA_CMSG_SPACE(sizeof(DWORD)) * 4]; WSACMSGHDR align; } ctl;
    WSAMSG msg;
    memset(&msg, 0, sizeof msg);
    msg.name = (LPSOCKADDR)&ss;
    msg.namelen = (INT)sizeof ss;
    msg.lpBuffers = &b;
    msg.dwBufferCount = 1;
    msg.Control.buf = ctl.b;
    msg.Control.len = (ULONG)sizeof ctl.b;
    DWORD n = 0;
    if (g_recvmsg(ns, &msg, &n, NULL, NULL) != 0) {
        const int e = net_last_error();
        if (e != WSAEMSGSIZE) return net_reason(e);
        /* The buffer was filled and the rest of the datagram discarded. */
        *received = (proven_size_t)clamp_len(cap);
        if (from) from_native(&ss, msg.namelen, from);
        return PROVEN_SYS_NET_TRUNCATED;
    }
    *received = (proven_size_t)n;
    if (from) from_native(&ss, msg.namelen, from);
    for (WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c)) {
        const int mark = ecn_of(c);
        if (mark >= 0) { if (ecn) *ecn = (proven_u8)mark; continue; }
        if (c->cmsg_level != IPPROTO_UDP || c->cmsg_type != UDP_COALESCED_INFO) continue;
        DWORD size = 0;
        memcpy(&size, WSA_CMSG_DATA(c), sizeof size);
        if (size > 0 && (proven_size_t)size < *received) *seg = (proven_size_t)size;
    }
    return (msg.dwFlags & MSG_TRUNC) ? PROVEN_SYS_NET_TRUNCATED : PROVEN_SYS_NET_OK;
#else
    return proven_sys_net_recv_from(sock, buf, cap, received, from);
#endif
}

proven_sys_net_result_t proven_sys_net_shutdown_write(proven_sys_socket_t sock) {
    if (shutdown((proven_native_socket_t)sock, NATIVE_SHUT_WR) != 0) return net_fail();
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_close(proven_sys_socket_t sock) {
    /* Not retried on EINTR: on Linux the descriptor is already gone, and closing it again could
     * close one that another thread has just been given. */
    if (native_close((proven_native_socket_t)sock) != 0) {
        int e = net_last_error();
        return net_interrupted(e) ? PROVEN_SYS_NET_OK : net_reason(e);
    }
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_local_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    proven_native_len_t len = (proven_native_len_t)sizeof ss;
    if (getsockname((proven_native_socket_t)sock, (struct sockaddr *)&ss, &len) != 0) return net_fail();
    from_native(&ss, len, out);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_peer_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    proven_native_len_t len = (proven_native_len_t)sizeof ss;
    if (getpeername((proven_native_socket_t)sock, (struct sockaddr *)&ss, &len) != 0) return net_fail();
    from_native(&ss, len, out);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_set_nodelay(proven_sys_socket_t sock, bool on) {
    int v = on ? 1 : 0;
    if (setsockopt((proven_native_socket_t)sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&v, sizeof v) != 0) return net_fail();
    return PROVEN_SYS_NET_OK;
}

proven_size_t proven_sys_net_wait_scratch_size(proven_size_t count) {
    if (count > PROVEN_SIZE_MAX / sizeof(proven_native_pollfd_t)) return PROVEN_SIZE_MAX;
    return count * sizeof(proven_native_pollfd_t);
}

proven_sys_net_result_t proven_sys_net_wait(proven_sys_net_wait_t *items, proven_size_t count,
                                            void *scratch, int timeout_ms, proven_size_t *ready) {
    *ready = 0;
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    proven_native_pollfd_t *fds = scratch;
    proven_size_t active = 0;
    for (proven_size_t i = 0; i < count; ++i) {
        fds[i].fd = (proven_native_socket_t)items[i].sock;
        fds[i].events = 0;
        fds[i].revents = 0;
        if (items[i].want & PROVEN_SYS_NET_READABLE) fds[i].events |= NATIVE_POLL_IN;
        if (items[i].want & PROVEN_SYS_NET_WRITABLE) fds[i].events |= NATIVE_POLL_OUT;
        /* A descriptor that is negative is passed over, by poll and by WSAPoll alike: the
         * way to keep an item in the list and out of the wait. */
        if (items[i].want & PROVEN_SYS_NET_IGNORE) { fds[i].fd = (proven_native_socket_t)-1; fds[i].events = 0; }
        else active++;
        items[i].got = 0;
    }
#if defined(_WIN32) || defined(_WIN64)
    if (active == 0) {
        /* WSAPoll refuses a set with nothing to wait on; such a wait is a sleep. */
        if (timeout_ms != 0) Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        return PROVEN_SYS_NET_OK;
    }
    int n = WSAPoll(fds, (ULONG)count, timeout_ms);
#else
    (void)active;
    int n = poll(fds, (nfds_t)count, timeout_ms);
#endif
    if (n < 0) {
        int e = net_last_error();
        return net_interrupted(e) ? PROVEN_SYS_NET_OK : net_reason(e);
    }
    for (proven_size_t i = 0; i < count; ++i) {
        /* (Of an item that was left out nothing is taken: poll leaves 0 there, WSAPoll may not.) */
        short r = (items[i].want & PROVEN_SYS_NET_IGNORE) ? 0 : fds[i].revents;
        proven_u8 got = 0;
        if (r & NATIVE_POLL_IN) got |= PROVEN_SYS_NET_READABLE;
        if (r & NATIVE_POLL_OUT) got |= PROVEN_SYS_NET_WRITABLE;
        /* A hang-up means the call that was asked about will not wait: a read returns the end
         * of input, a write its error. Windows reports a peer's orderly close as a hang-up
         * alone, where Linux reports it as readable; both must read the same to the caller. */
        if (r & POLLHUP) got |= (proven_u8)(items[i].want & (PROVEN_SYS_NET_READABLE | PROVEN_SYS_NET_WRITABLE));
        if (r & (POLLERR | POLLNVAL)) got |= PROVEN_SYS_NET_FAILED;
        /* Whatever else the OS reported, something happened: an item that woke the wait must
         * not look idle, or the caller would wait again and spin. */
        if (r != 0 && got == 0) got = PROVEN_SYS_NET_FAILED;
        items[i].got = got;
        if (got) (*ready)++;
    }
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_resolve(const char *host, proven_u16 port,
                                               proven_sys_net_addr_t *out, proven_size_t cap,
                                               proven_size_t *count) {
    *count = 0;
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &list);
    if (rc != 0) {
        if (list) freeaddrinfo(list);
        switch (rc) {
            case EAI_NONAME:
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
            case EAI_NODATA:
#endif
                return PROVEN_SYS_NET_NOT_FOUND;
            case EAI_AGAIN:  return PROVEN_SYS_NET_TIMEOUT;   /* the name server did not answer */
            case EAI_MEMORY: return PROVEN_SYS_NET_LIMIT;
            default:         return PROVEN_SYS_NET_ERROR;
        }
    }
    for (struct addrinfo *ai = list; ai && *count < cap; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        proven_size_t n = (proven_size_t)ai->ai_addrlen < sizeof ss ? (proven_size_t)ai->ai_addrlen : sizeof ss;
        memcpy(&ss, ai->ai_addr, n);
        from_native(&ss, (proven_native_len_t)n, &out[*count]);
        out[*count].port = port;
        (*count)++;
    }
    freeaddrinfo(list);
    return *count > 0 ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_NOT_FOUND;
}

// -----------------------------------------------------------------------------
// Selector: epoll, kqueue, or none
// -----------------------------------------------------------------------------

#if defined(__linux__)
#include <sys/epoll.h>

proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void) { return PROVEN_SYS_NET_SELECTOR_EPOLL; }

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out) {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) return net_reason(errno);
    *out = (proven_uintptr_t)fd;
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_selector_close(proven_uintptr_t selector) {
    (void)close((int)selector);
}

proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    if (want & PROVEN_SYS_NET_READABLE) ev.events |= EPOLLIN;
    if (want & PROVEN_SYS_NET_WRITABLE) ev.events |= EPOLLOUT;
    /* An error and a hang-up are reported whether asked for or not, and for as long as they
     * last. Of a socket of which nothing is asked they are reported once: its owner is not
     * looking - a request that is paused - and a wait that returned at once every time would
     * spin. When something is asked again, they are reported again. */
    if (ev.events == 0) ev.events = EPOLLET;
    ev.data.ptr = tag;
    if (epoll_ctl((int)selector, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, (int)sock, &ev) == 0) return PROVEN_SYS_NET_OK;
    if (errno == EEXIST) return PROVEN_SYS_NET_EXISTS;
    if (errno == ENOENT) return PROVEN_SYS_NET_NOT_FOUND;
    if (errno == ENOSPC || errno == ENOMEM) return PROVEN_SYS_NET_LIMIT;
    return net_reason(errno);
}

proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    if (epoll_ctl((int)selector, EPOLL_CTL_DEL, (int)sock, &ev) == 0) return PROVEN_SYS_NET_OK;
    return errno == ENOENT ? PROVEN_SYS_NET_NOT_FOUND : net_reason(errno);
}

proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count) {
    struct epoll_event got[64];
    *count = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    int want = cap < 64 ? (int)cap : 64;
    int n = epoll_wait((int)selector, got, want, timeout_ms);
    if (n < 0) return errno == EINTR ? PROVEN_SYS_NET_OK : net_reason(errno);
    for (int i = 0; i < n; ++i) {
        proven_u8 g = 0;
        if (got[i].events & EPOLLIN) g |= PROVEN_SYS_NET_READABLE;
        if (got[i].events & EPOLLOUT) g |= PROVEN_SYS_NET_WRITABLE;
        /* A hang-up means neither a read nor a write will wait; which was asked is not known
         * here, so both are reported and the call that follows says what happened. */
        if (got[i].events & EPOLLHUP) g |= PROVEN_SYS_NET_READABLE | PROVEN_SYS_NET_WRITABLE;
        if (got[i].events & EPOLLERR) g |= PROVEN_SYS_NET_FAILED;
        if (g == 0) g = PROVEN_SYS_NET_FAILED;
        events[i].tag = got[i].data.ptr;
        events[i].got = g;
    }
    *count = (proven_size_t)n;
    return PROVEN_SYS_NET_OK;
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#include <sys/time.h>

/* Written from the manual pages. Like every BSD path in this unit it has not been compiled or
 * run by this project: no such host is available to it. */

proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void) { return PROVEN_SYS_NET_SELECTOR_KQUEUE; }

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out) {
    int fd = kqueue();
    if (fd < 0) return net_reason(errno);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    *out = (proven_uintptr_t)fd;
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_selector_close(proven_uintptr_t selector) {
    (void)close((int)selector);
}

/* kqueue keeps one entry per (socket, filter). Each change is applied by itself so that
 * deleting a filter that was never added - which is not an error for this interface - can be
 * told from a real failure. */
static int kq_change(int kq, int sock, int filter, int flags, void *tag) {
    struct kevent ch;
    EV_SET(&ch, (uintptr_t)sock, filter, flags, 0, 0, tag);
    return kevent(kq, &ch, 1, NULL, 0, NULL) == 0 ? 0 : errno;
}

proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add) {
    (void)add;      /* kqueue has no separate "modify": adding an existing filter replaces it */
    int kq = (int)selector;
    int e1 = kq_change(kq, (int)sock, EVFILT_READ, (want & PROVEN_SYS_NET_READABLE) ? EV_ADD : EV_DELETE, tag);
    int e2 = kq_change(kq, (int)sock, EVFILT_WRITE, (want & PROVEN_SYS_NET_WRITABLE) ? EV_ADD : EV_DELETE, tag);
    if (e1 != 0 && e1 != ENOENT) return net_reason(e1);
    if (e2 != 0 && e2 != ENOENT) return net_reason(e2);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock) {
    int kq = (int)selector;
    int e1 = kq_change(kq, (int)sock, EVFILT_READ, EV_DELETE, NULL);
    int e2 = kq_change(kq, (int)sock, EVFILT_WRITE, EV_DELETE, NULL);
    if (e1 != 0 && e1 != ENOENT) return net_reason(e1);
    if (e2 != 0 && e2 != ENOENT) return net_reason(e2);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count) {
    struct kevent got[64];
    struct timespec ts, *tp = NULL;
    *count = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent((int)selector, NULL, 0, got, cap < 64 ? (int)cap : 64, tp);
    if (n < 0) return errno == EINTR ? PROVEN_SYS_NET_OK : net_reason(errno);
    for (int i = 0; i < n; ++i) {
        proven_u8 g = 0;
        if (got[i].flags & EV_ERROR) g = PROVEN_SYS_NET_FAILED;
        else if (got[i].filter == EVFILT_READ) g = PROVEN_SYS_NET_READABLE;
        else if (got[i].filter == EVFILT_WRITE) g = PROVEN_SYS_NET_WRITABLE;
        else g = PROVEN_SYS_NET_FAILED;
        events[i].tag = got[i].udata;
        events[i].got = g;
    }
    *count = (proven_size_t)n;
    return PROVEN_SYS_NET_OK;
}

#else

proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void) { return PROVEN_SYS_NET_SELECTOR_NONE; }

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out) {
    (void)out;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

void proven_sys_net_selector_close(proven_uintptr_t selector) { (void)selector; }

proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add) {
    (void)selector; (void)sock; (void)want; (void)tag; (void)add;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock) {
    (void)selector; (void)sock;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count) {
    (void)selector; (void)events; (void)cap; (void)timeout_ms;
    *count = 0;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

#endif

#if defined(_WIN32) || defined(_WIN64)
/* ============================================================================
 * I/O completion ports (RFC-0025 step 6), for the event loop
 * ========================================================================== */

_Static_assert(sizeof(proven_sys_net_ov_t) == sizeof(OVERLAPPED), "proven_sys_net_ov_t is an OVERLAPPED");
_Static_assert(_Alignof(proven_sys_net_ov_t) >= _Alignof(OVERLAPPED), "and aligned as one");
_Static_assert(PROVEN_SYS_NET_ACCEPT_AREA >= 2 * (sizeof(struct sockaddr_storage) + 16), "room for AcceptEx's two addresses");
_Static_assert(PROVEN_SYS_NET_VEC_MAX <= 64, "a send's buffers are on the stack");

/* What a datagram receive keeps while in flight: the system writes the message's lengths and
 * flags, the sender and the control data when the receive finishes, not when it starts. */
typedef struct {
    WSAMSG msg;
    WSABUF buf;
    INT fromlen;                  /* for WSARecvFrom, where there is no WSARecvMsg */
    DWORD flags;
    struct sockaddr_storage from;
    union { char b[WSA_CMSG_SPACE(sizeof(DWORD)) * 4]; WSACMSGHDR align; } ctl;
    bool plain;                   /* received with WSARecvFrom: no segment size */
} port_msg_t;
_Static_assert(sizeof(port_msg_t) <= sizeof(proven_sys_net_msg_t), "proven_sys_net_msg_t holds a datagram receive");
_Static_assert(_Alignof(port_msg_t) <= _Alignof(proven_sys_net_msg_t), "and is aligned for it");

/* AcceptEx and ConnectEx are reached through pointers the provider hands out: fetched once for
 * the IP families, with the first socket that asks; a Unix-domain socket asks its own provider
 * each time (rare, and its pointers may differ). */
static INIT_ONCE g_ext_once = INIT_ONCE_STATIC_INIT;
static LPFN_ACCEPTEX g_acceptex;
static LPFN_CONNECTEX g_connectex;

static void ext_fetch(SOCKET s, LPFN_ACCEPTEX *acc, LPFN_CONNECTEX *con) {
    GUID a = WSAID_ACCEPTEX, c = WSAID_CONNECTEX;
    DWORD n = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &a, (DWORD)sizeof a, acc, (DWORD)sizeof *acc, &n, NULL, NULL) != 0) *acc = NULL;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &c, (DWORD)sizeof c, con, (DWORD)sizeof *con, &n, NULL, NULL) != 0) *con = NULL;
}

static BOOL CALLBACK ext_find(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)context;
    ext_fetch(*(const SOCKET *)param, &g_acceptex, &g_connectex);
    return TRUE;
}

static void ext_for(SOCKET s, proven_sys_net_family_t family, LPFN_ACCEPTEX *acc, LPFN_CONNECTEX *con) {
    if (family == PROVEN_SYS_NET_FAMILY_UNIX) { ext_fetch(s, acc, con); return; }
    InitOnceExecuteOnce(&g_ext_once, ext_find, &s, NULL);
    *acc = g_acceptex;
    *con = g_connectex;
}

/* Reasons an operation of the port fails with, beside the socket errors: the system's own codes,
 * which WSAGetOverlappedResult does not always turn into socket errors. */
static proven_sys_net_result_t ov_reason(int e) {
    switch (e) {
        case WSA_OPERATION_ABORTED:      return PROVEN_SYS_NET_CANCELLED;
        case ERROR_NETNAME_DELETED:
        case ERROR_CONNECTION_ABORTED:   return PROVEN_SYS_NET_RESET;
        case ERROR_CONNECTION_REFUSED:   return PROVEN_SYS_NET_REFUSED;
        case ERROR_SEM_TIMEOUT:          return PROVEN_SYS_NET_TIMEOUT;
        case ERROR_NETWORK_UNREACHABLE:
        case ERROR_HOST_UNREACHABLE:     return PROVEN_SYS_NET_UNREACHABLE;
        case ERROR_MORE_DATA:
        case WSAEMSGSIZE:                return PROVEN_SYS_NET_TRUNCATED;    /* a receive: the datagram was cut */
        default:                         return net_reason(e);
    }
}

/* What a start's return says: done at once, under way (the port will report it), or failed. */
static proven_sys_net_result_t ov_started(bool done_now, bool skip) {
    if (done_now) return skip ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_IN_PROGRESS;
    const int e = WSAGetLastError();
    if (e == WSA_IO_PENDING) return PROVEN_SYS_NET_IN_PROGRESS;
    /* A datagram cut short is a warning of the system's, not a failure: the receive has finished
     * and the port reports it (as failed), which a start that failed at once would not be. */
    if (e == WSAEMSGSIZE && !skip) return PROVEN_SYS_NET_IN_PROGRESS;
    return ov_reason(e);
}

bool proven_sys_net_port_available(void) { return true; }
bool proven_sys_net_port_preferred(void) { return true; }

proven_sys_net_result_t proven_sys_net_port_open(proven_uintptr_t *out) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    HANDLE h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!h) return PROVEN_SYS_NET_LIMIT;
    *out = (proven_uintptr_t)h;
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_port_close(proven_uintptr_t port) { CloseHandle((HANDLE)port); }

proven_sys_net_result_t proven_sys_net_port_bind(proven_uintptr_t port, proven_sys_socket_t sock, bool allow_skip, bool *skip) {
    *skip = false;
    if (!CreateIoCompletionPort((HANDLE)sock, (HANDLE)port, 0, 0)) {
        /* Bound already: a handle is bound once for its life, and the caller's rule is one loop
         * per socket. */
        if (GetLastError() != ERROR_INVALID_PARAMETER) return PROVEN_SYS_NET_ERROR;
    }
    /* Not reporting what finishes at once is safe only when every provider on the socket's path
     * hands out real handles (no layered provider of the old kind). Asked again for a socket bound
     * already: the answer is the same, and the caller learns it. */
    if (!allow_skip) return PROVEN_SYS_NET_OK;
    WSAPROTOCOL_INFOW info;
    int len = (int)sizeof info;
    if (getsockopt((SOCKET)sock, SOL_SOCKET, SO_PROTOCOL_INFOW, (char *)&info, &len) == 0 && (info.dwServiceFlags1 & XP1_IFS_HANDLES) &&
        SetFileCompletionNotificationModes((HANDLE)sock, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
        *skip = true;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_port_post(proven_uintptr_t port) {
    return PostQueuedCompletionStatus((HANDLE)port, 0, 0, NULL) ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_LIMIT;
}

proven_sys_net_result_t proven_sys_net_port_wait(proven_uintptr_t port, proven_sys_net_done_t *out, proven_size_t cap,
                                                 int timeout_ms, proven_size_t *count) {
    OVERLAPPED_ENTRY got[64];
    *count = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    if (cap > 64) cap = 64;
    ULONG n = 0;
    if (!GetQueuedCompletionStatusEx((HANDLE)port, got, (ULONG)cap, &n, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, FALSE)) {
        const DWORD e = GetLastError();
        return e == WAIT_TIMEOUT ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_ERROR;
    }
    for (ULONG i = 0; i < n; ++i) {
        OVERLAPPED *o = got[i].lpOverlapped;
        out[i].ov = (proven_sys_net_ov_t *)(void *)o;
        out[i].bytes = (proven_u32)got[i].dwNumberOfBytesTransferred;
        /* `Internal` is the request's status once it has completed (an NTSTATUS: negative is a
         * failure, a datagram cut short among them). */
        out[i].failed = o && (LONG)(ULONG)o->Internal < 0;
        out[i].more = false;
    }
    *count = (proven_size_t)n;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_ov_result(proven_sys_socket_t sock, proven_sys_net_ov_t *ov) {
    OVERLAPPED *o = (OVERLAPPED *)(void *)ov;
    if ((ULONG)o->Internal == 0xC0000120u) return PROVEN_SYS_NET_CANCELLED;      /* STATUS_CANCELLED */
    DWORD n = 0, flags = 0;
    if (WSAGetOverlappedResult((SOCKET)sock, o, &n, FALSE, &flags)) return PROVEN_SYS_NET_OK;
    return ov_reason(WSAGetLastError());
}

void proven_sys_net_ov_cancel(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov) {
    (void)port;
    (void)CancelIoEx((HANDLE)sock, (OVERLAPPED *)(void *)ov);
}

proven_sys_net_result_t proven_sys_net_ov_recv(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, void *buf, proven_size_t cap,
                                               bool skip, proven_u32 *bytes) {
    (void)port;
    /* The WSABUF may be on the stack: the provider takes it before the call returns. */
    WSABUF b = { .len = cap > 0 ? (ULONG)clamp_len(cap) : 0, .buf = (CHAR *)buf };
    DWORD n = 0, flags = 0;
    memset(ov, 0, sizeof *ov);
    const bool now = WSARecv((SOCKET)sock, &b, 1, &n, &flags, (OVERLAPPED *)(void *)ov, NULL) == 0;
    *bytes = (proven_u32)n;
    return ov_started(now, skip);
}

proven_sys_net_result_t proven_sys_net_ov_send(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, proven_sys_net_keep_t *keep,
                                               const proven_sys_net_vec_t *vec, proven_size_t count, bool skip, proven_u32 *bytes) {
    (void)port;
    (void)keep;                         /* the provider takes the WSABUFs before the call returns */
    WSABUF b[PROVEN_SYS_NET_VEC_MAX];
    if (count > PROVEN_SYS_NET_VEC_MAX) count = PROVEN_SYS_NET_VEC_MAX;
    for (proven_size_t i = 0; i < count; ++i) { b[i].len = (ULONG)clamp_len(vec[i].len); b[i].buf = (CHAR *)(proven_uintptr_t)vec[i].ptr; }
    DWORD n = 0;
    memset(ov, 0, sizeof *ov);
    const bool now = WSASend((SOCKET)sock, b, (DWORD)count, &n, 0, (OVERLAPPED *)(void *)ov, NULL) == 0;
    *bytes = (proven_u32)n;
    return ov_started(now, skip);
}

proven_sys_net_result_t proven_sys_net_ov_accept(proven_uintptr_t port, proven_sys_socket_t listener, proven_sys_net_family_t family, proven_sys_net_ov_t *ov,
                                                 void *area, bool skip, proven_sys_socket_t *accepted) {
    (void)port;
    *accepted = PROVEN_SYS_SOCKET_INVALID;
    LPFN_ACCEPTEX accept_ex = NULL;
    LPFN_CONNECTEX connect_ex = NULL;
    ext_for((SOCKET)listener, family, &accept_ex, &connect_ex);
    if (!accept_ex) return PROVEN_SYS_NET_UNSUPPORTED;
    const int af = native_family(family);
    if (af < 0) return PROVEN_SYS_NET_ADDR_INVALID;
    SOCKET s = native_open(af, SOCK_STREAM);
    if (s == INVALID_SOCKET) return net_fail();
    const DWORD each = (DWORD)(sizeof(struct sockaddr_storage) + 16);
    DWORD n = 0;
    memset(ov, 0, sizeof *ov);
    /* No data with the accept: a client that connects and says nothing would hold it. */
    const bool now = accept_ex((SOCKET)listener, s, area, 0, each, each, &n, (OVERLAPPED *)(void *)ov) != FALSE;
    const proven_sys_net_result_t r = ov_started(now, skip);
    if (r != PROVEN_SYS_NET_OK && r != PROVEN_SYS_NET_IN_PROGRESS) { closesocket(s); return r; }
    *accepted = (proven_sys_socket_t)s;
    return r;
}

proven_sys_net_result_t proven_sys_net_ov_accept_done(proven_sys_socket_t listener, proven_sys_socket_t accepted) {
    SOCKET l = (SOCKET)listener;
    if (setsockopt((SOCKET)accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&l, (int)sizeof l) != 0) return net_fail();
    return native_prepare_accepted((SOCKET)accepted) ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_ERROR;
}

proven_sys_net_result_t proven_sys_net_ov_connect_socket(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    const int af = native_family(addr->family);
    if (af < 0) return PROVEN_SYS_NET_ADDR_INVALID;
    SOCKET s = native_open(af, SOCK_STREAM);
    if (s == INVALID_SOCKET) return net_fail();
    if (af != AF_UNIX) {
        /* ConnectEx connects only a socket that is bound. */
        struct sockaddr_storage any;
        memset(&any, 0, sizeof any);
        any.ss_family = (ADDRESS_FAMILY)af;
        const int len = af == AF_INET ? (int)sizeof(struct sockaddr_in) : (int)sizeof(struct sockaddr_in6);
        if (bind(s, (const struct sockaddr *)&any, len) != 0) {
            const proven_sys_net_result_t r = net_fail();
            closesocket(s);
            return r;
        }
    }
    *out = (proven_sys_socket_t)s;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_ov_connect(proven_uintptr_t port, proven_sys_socket_t sock, const proven_sys_net_addr_t *addr, proven_sys_net_ov_t *ov,
                                                  bool skip) {
    (void)port;
    struct sockaddr_storage ss;
    const proven_native_len_t len = to_native(addr, &ss);
    if (len == 0) return PROVEN_SYS_NET_ADDR_INVALID;
    if (addr->family == PROVEN_SYS_NET_FAMILY_UNIX) {
        /* Not an operation of the port: a local connect, made blocking for its moment. */
        u_long off = 0, on = 1;
        (void)ioctlsocket((SOCKET)sock, FIONBIO, &off);
        const int rc = connect((SOCKET)sock, (const struct sockaddr *)&ss, len);
        const proven_sys_net_result_t r = rc == 0 ? PROVEN_SYS_NET_OK : net_fail();
        (void)ioctlsocket((SOCKET)sock, FIONBIO, &on);
        return r;
    }
    LPFN_ACCEPTEX accept_ex = NULL;
    LPFN_CONNECTEX connect_ex = NULL;
    ext_for((SOCKET)sock, addr->family, &accept_ex, &connect_ex);
    if (!connect_ex) return PROVEN_SYS_NET_UNSUPPORTED;
    DWORD n = 0;
    memset(ov, 0, sizeof *ov);
    const bool now = connect_ex((SOCKET)sock, (const struct sockaddr *)&ss, len, NULL, 0, &n, (OVERLAPPED *)(void *)ov) != FALSE;
    const proven_sys_net_result_t r = ov_started(now, skip);
    return r == PROVEN_SYS_NET_OK ? proven_sys_net_ov_connect_done(sock) : r;
}

proven_sys_net_result_t proven_sys_net_ov_connect_done(proven_sys_socket_t sock) {
    /* Without this the socket does not know it is connected: getpeername and shutdown fail. */
    if (setsockopt((SOCKET)sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) return net_fail();
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_ov_recv_msg(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, proven_sys_net_msg_t *msg,
                                                   void *buf, proven_size_t cap, bool skip, proven_u32 *bytes) {
    (void)port;
    port_msg_t *m = (port_msg_t *)(void *)msg;
    SOCKET s = (SOCKET)sock;
    InitOnceExecuteOnce(&g_recvmsg_once, recvmsg_find, &s, NULL);
    memset(m, 0, sizeof *m);
    m->buf.len = (ULONG)clamp_len(cap);
    m->buf.buf = (CHAR *)buf;
    DWORD n = 0;
    bool now;
    memset(ov, 0, sizeof *ov);
    if (!g_recvmsg || atomic_load_explicit(&g_segments_off, memory_order_relaxed)) {
        m->plain = true;
        m->fromlen = (INT)sizeof m->from;
        now = WSARecvFrom(s, &m->buf, 1, &n, &m->flags, (struct sockaddr *)&m->from, &m->fromlen, (OVERLAPPED *)(void *)ov, NULL) == 0;
    } else {
        m->msg.name = (LPSOCKADDR)&m->from;
        m->msg.namelen = (INT)sizeof m->from;
        m->msg.lpBuffers = &m->buf;
        m->msg.dwBufferCount = 1;
        m->msg.Control.buf = m->ctl.b;
        m->msg.Control.len = (ULONG)sizeof m->ctl.b;
        now = g_recvmsg(s, &m->msg, &n, (OVERLAPPED *)(void *)ov, NULL) == 0;
    }
    *bytes = (proven_u32)n;
    return ov_started(now, skip);
}

void proven_sys_net_ov_msg_done(const proven_sys_net_msg_t *msg, proven_u32 bytes, proven_size_t *seg, proven_u8 *ecn, proven_sys_net_addr_t *from) {
    const port_msg_t *m = (const port_msg_t *)(const void *)msg;
    *seg = 0;
    if (ecn) *ecn = 0;
    if (from) from_native(&m->from, m->plain ? m->fromlen : m->msg.namelen, from);
    if (m->plain) return;
    WSAMSG copy = m->msg;
    for (WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&copy); c; c = WSA_CMSG_NXTHDR(&copy, c)) {
        if (c->cmsg_len < sizeof(WSACMSGHDR)) break;          /* nothing written there */
        const int mark = ecn_of(c);
        if (mark >= 0) { if (ecn) *ecn = (proven_u8)mark; continue; }
        if (c->cmsg_level != IPPROTO_UDP || c->cmsg_type != UDP_COALESCED_INFO) continue;
        DWORD size = 0;
        memcpy(&size, WSA_CMSG_DATA(c), sizeof size);
        if (size > 0 && (proven_size_t)size < bytes) *seg = (proven_size_t)size;
    }
}

proven_sys_net_result_t proven_sys_net_port_attach(proven_uintptr_t port, proven_sys_socket_t sock) {
    if (CreateIoCompletionPort((HANDLE)sock, (HANDLE)port, 0, 0)) return PROVEN_SYS_NET_OK;
    return GetLastError() == ERROR_INVALID_PARAMETER ? PROVEN_SYS_NET_EXISTS : PROVEN_SYS_NET_ERROR;
}

proven_sys_net_result_t proven_sys_net_sock_kind(proven_sys_socket_t sock, bool *stream, bool *listening) {
    int type = 0, acc = 0;
    int len = (int)sizeof type;
    *stream = *listening = false;
    if (getsockopt((SOCKET)sock, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0) return net_fail();
    len = (int)sizeof acc;
    if (getsockopt((SOCKET)sock, SOL_SOCKET, SO_ACCEPTCONN, (char *)&acc, &len) != 0) acc = 0;
    *stream = type == SOCK_STREAM;
    *listening = acc != 0;
    return PROVEN_SYS_NET_OK;
}

/* The words of a proven_sys_net_evt_t. */
typedef struct {
    HANDLE event;               /* auto-reset: the pool's wait takes the signal */
    PTP_WAIT wait;
    HANDLE port;
    OVERLAPPED *marker;
    volatile LONG posted;       /* the pool posted the marker; the port has not reported it yet */
} port_evt_t;
_Static_assert(sizeof(port_evt_t) <= sizeof(proven_sys_net_evt_t), "proven_sys_net_evt_t holds an event, a wait and a marker");

static VOID CALLBACK evt_fired(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WAIT wait, TP_WAIT_RESULT result) {
    (void)inst; (void)wait; (void)result;
    port_evt_t *e = ctx;
    InterlockedExchange(&e->posted, 1);
    /* Out of memory for the packet: the notice is lost, as the caller's next arm would find out. */
    if (!PostQueuedCompletionStatus(e->port, 0, 0, e->marker)) InterlockedExchange(&e->posted, 0);
}

proven_sys_net_result_t proven_sys_net_evt_open(proven_sys_net_evt_t *evt, proven_uintptr_t port, proven_sys_net_ov_t *marker) {
    port_evt_t *e = (port_evt_t *)(void *)evt;
    memset(evt, 0, sizeof *evt);
    e->port = (HANDLE)port;
    e->marker = (OVERLAPPED *)(void *)marker;
    e->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!e->event) return PROVEN_SYS_NET_LIMIT;
    e->wait = CreateThreadpoolWait(evt_fired, e, NULL);
    if (!e->wait) { CloseHandle(e->event); e->event = NULL; return PROVEN_SYS_NET_LIMIT; }
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_evt_arm(proven_sys_net_evt_t *evt, proven_sys_socket_t sock, proven_u8 want) {
    port_evt_t *e = (port_evt_t *)(void *)evt;
    long mask = FD_CLOSE;
    /* A connect is asked about only while there is one: of a socket connected long ago the
     * system might record it on every arm. */
    struct sockaddr_storage peer;
    int plen = (int)sizeof peer;
    if (getpeername((SOCKET)sock, (struct sockaddr *)&peer, &plen) != 0 && WSAGetLastError() == WSAENOTCONN) mask |= FD_CONNECT;
    if (want & PROVEN_SYS_NET_READABLE) mask |= FD_READ | FD_ACCEPT;
    if (want & PROVEN_SYS_NET_WRITABLE) mask |= FD_WRITE;
    /* A new selection clears the socket's record and records what holds now, so a condition
     * that held before the arm is noticed. */
    if (WSAEventSelect((SOCKET)sock, e->event, mask) != 0) return net_fail();
    SetThreadpoolWait(e->wait, e->event, NULL);
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_evt_taken(proven_sys_net_evt_t *evt) {
    InterlockedExchange(&((port_evt_t *)(void *)evt)->posted, 0);
}

bool proven_sys_net_evt_disarm(proven_sys_net_evt_t *evt, proven_sys_socket_t sock) {
    port_evt_t *e = (port_evt_t *)(void *)evt;
    if (!e->wait) return false;
    SetThreadpoolWait(e->wait, NULL, NULL);
    WaitForThreadpoolWaitCallbacks(e->wait, TRUE);
    if (sock != PROVEN_SYS_SOCKET_INVALID) (void)WSAEventSelect((SOCKET)sock, NULL, 0);
    ResetEvent(e->event);
    return InterlockedCompareExchange(&e->posted, 0, 0) != 0;
}

void proven_sys_net_evt_close(proven_sys_net_evt_t *evt) {
    port_evt_t *e = (port_evt_t *)(void *)evt;
    if (e->wait) {
        (void)proven_sys_net_evt_disarm(evt, PROVEN_SYS_SOCKET_INVALID);
        CloseThreadpoolWait(e->wait);
    }
    if (e->event) CloseHandle(e->event);
    memset(evt, 0, sizeof *evt);
}
#endif

#if PROVEN_SYS_RING
/* ---- The port on Linux: io_uring (RFC-0029) ----
 *
 * One ring per port, mapped into the process: the loop writes entries into its submission queue
 * and reads reports from its completion queue; one system call submits what was written and
 * waits for reports. Raw system calls, no liburing. What a report carries back is the entry's
 * `user_data`: the OVERLAPPED's address (proven_sys_net_ov_t), or RING_WAKE for the waker - an
 * eventfd with a read always in flight - or RING_IGNORE for an entry whose report means nothing
 * (a cancel's). The OVERLAPPED keeps the report's result and what kind of entry it was. */

#include <linux/io_uring.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <limits.h>
#include <stdlib.h>

#ifndef IORING_ASYNC_CANCEL_ALL
#define IORING_ASYNC_CANCEL_ALL (1U << 0)
#endif
#ifndef IORING_ASYNC_CANCEL_ANY
#define IORING_ASYNC_CANCEL_ANY (1U << 2)
#endif
#ifndef IORING_ASYNC_CANCEL_FD
#define IORING_ASYNC_CANCEL_FD (1U << 1)
#endif
#ifndef IORING_FEAT_EXT_ARG
#define IORING_FEAT_EXT_ARG (1U << 8)
#endif
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 3)
#endif
#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif

/* Operation numbers are an enum in the header; these are its values (a stable ABI). */
enum { RING_OP_POLL_ADD = 6, RING_OP_SENDMSG = 9, RING_OP_RECVMSG = 10, RING_OP_ACCEPT = 13, RING_OP_ASYNC_CANCEL = 14,
       RING_OP_READ = 22, RING_OP_RECV = 27, RING_OP_SOCKET = 45, RING_OP_SEND_ZC = 47 };
#define RING_REGISTER_PROBE 8u

/* Provided buffers and multishot receives (RFC-0030), as the header of 6.0 has them; ours, so
 * that an older header builds too. A kernel of 6.0 is told by an operation it added (SEND_ZC):
 * multishot receives have no probe of their own. */
#define RING_REGISTER_PBUF_RING 22u
#define RING_RECV_MULTISHOT (1U << 1)            /* IORING_RECV_MULTISHOT, in the entry's ioprio */
#define RING_SQE_BUFFER_SELECT (1U << 5)         /* IOSQE_BUFFER_SELECT */
#define RING_CQE_F_BUFFER (1U << 0)
#define RING_CQE_F_MORE (1U << 1)
#define RING_CQE_BUFFER_SHIFT 16
#define RING_POOL_GROUP 1u
typedef struct { proven_u64 addr; proven_u32 len; proven_u16 bid, resv; } ring_buf_t;   /* the ring's tail is entry 0's resv */
typedef struct { proven_u64 ring_addr; proven_u32 ring_entries; proven_u16 bgid, flags; proven_u64 resv[3]; } ring_buf_reg_t;

/* The probe's layout (struct io_uring_probe, 5.6): ours, so that an older header builds too. */
typedef struct {
    proven_u8 last_op, ops_len;
    proven_u16 resv;
    proven_u32 resv2[3];
    struct { proven_u8 op, resv; proven_u16 flags; proven_u32 resv2; } ops[256];
} ring_probe_t;

typedef struct {
    proven_u64 sigmask;
    proven_u32 sigmask_sz, pad;
    proven_u64 ts;
} ring_getevents_arg_t;

#define RING_WAKE ((proven_u64)0)
#define RING_IGNORE ((proven_u64)1)
#define RING_MULTI ((proven_u64)2)              /* or'd into a multishot receive's OVERLAPPED address (at least 4-aligned) */
#define RING_TRUNCATED INT_MIN                  /* a datagram cut short: what ov_result says TRUNCATED for */

/* What an OVERLAPPED holds here, one word: the kind of entry in its low two bits; above them,
 * while in flight, a datagram receive's message (whose flags say whether the datagram was cut; a
 * msghdr is aligned to at least four bytes), and once reported, the result in the high half. */
enum { RK_IO = 1, RK_POLL, RK_MSG };
#define RK_KIND(ov) ((unsigned)((ov)->w & 3u))
#define RK_HDR(ov) ((const struct msghdr *)(proven_uintptr_t)((ov)->w & ~(proven_u64)3u))

/* A multishot receive's OVERLAPPED is not a kind and a result but the socket's filled buffers, a
 * list through the pool's `next` links: the first and last buffer, how much of the first is taken,
 * whether there are any; whether the receive is in flight, whether it has ended and why. */
#define MS_HEAD(w) ((proven_u16)((w) & 0xffffu))
#define MS_TAIL(w) ((proven_u16)(((w) >> 16) & 0xffffu))
#define MS_OFF(w) ((proven_u32)(((w) >> 32) & 0xffffu))
#define MS_HELD ((proven_u64)1 << 48)
#define MS_ARMED ((proven_u64)1 << 49)
#define MS_ENDED ((proven_u64)1 << 50)
#define MS_REASON(w) ((proven_sys_net_result_t)(((w) >> 56) & 0xffu))
#define MS_LIST(head, tail, off) ((proven_u64)(head) | ((proven_u64)(tail) << 16) | ((proven_u64)(off) << 32))
#define MS_LIST_MASK ((proven_u64)0xffffffffffffu)

typedef struct {
    int fd, efd;
    proven_u32 sq_entries, cq_entries, to_submit;
    void *sq_map, *cq_map;
    size_t sq_size, cq_size;
    struct io_uring_sqe *sqes;
    size_t sqes_size;
    proven_u32 *sq_head, *sq_tail, *sq_mask, *sq_array;
    proven_u32 *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
    proven_u64 wake_buf;                        /* the eventfd read's target */
    /* The pool of provided buffers (RFC-0030), made at the first need. */
    bool pool_tried, pool_ok;
    proven_u16 pool_tail;                       /* where the next buffer given back goes in the buffer ring */
    proven_u32 pool_count, pool_bytes, pool_held;   /* pool_held: filled buffers not yet given back */
    ring_buf_t *pool_ring;
    size_t pool_ring_size, pool_mem_size;
    proven_byte_t *pool_mem;
    proven_u32 *pool_len;                       /* per buffer: what the kernel put in it */
    proven_u16 *pool_next;                      /* per buffer: the next of the same socket's list */
} ring_t;

typedef struct {
    struct msghdr m;
    struct iovec iov;
    struct sockaddr_storage from;
    union { char b[CMSG_SPACE(sizeof(int)) * 4]; struct cmsghdr align; } ctl;
} ring_msg_t;
_Static_assert(sizeof(ring_msg_t) <= sizeof(proven_sys_net_msg_t), "a datagram receive's message fits its keep");

typedef struct {
    struct msghdr m;
    struct iovec iov[PROVEN_SYS_NET_VEC_MAX];
} ring_keep_t;
_Static_assert(sizeof(ring_keep_t) <= sizeof(proven_sys_net_keep_t), "a send's message fits its keep");

static int ring_setup(unsigned entries, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, entries, p); }
static int ring_enter(int fd, unsigned submit, unsigned min_complete, unsigned flags, const void *arg, size_t argsz) {
    return (int)syscall(__NR_io_uring_enter, fd, submit, min_complete, flags, arg, argsz);
}

static void ring_unmap(ring_t *r) {
    if (r->sqes && r->sqes != MAP_FAILED) munmap(r->sqes, r->sqes_size);
    if (r->cq_map && r->cq_map != MAP_FAILED && r->cq_map != r->sq_map) munmap(r->cq_map, r->cq_size);
    if (r->sq_map && r->sq_map != MAP_FAILED) munmap(r->sq_map, r->sq_size);
}

/* A ring of `sq` entries and `cq` reports, mapped; false when the kernel refuses. */
static bool ring_open(ring_t *r, unsigned sq, unsigned cq) {
    memset(r, 0, sizeof *r);
    r->fd = r->efd = -1;
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN;
    p.cq_entries = cq;
    int fd = ring_setup(sq, &p);
    if (fd < 0 && errno == EINVAL) {                /* an older kernel: without the flags of 5.18 and 5.19 */
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_CQSIZE;
        p.cq_entries = cq;
        fd = ring_setup(sq, &p);
    }
    if (fd < 0) return false;
    r->fd = fd;
    r->sq_entries = p.sq_entries;
    r->cq_entries = p.cq_entries;
    r->sq_size = p.sq_off.array + p.sq_entries * sizeof(proven_u32);
    r->cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (r->cq_size > r->sq_size) r->sq_size = r->cq_size;
        r->cq_size = r->sq_size;
    }
    r->sq_map = mmap(NULL, r->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (r->sq_map == MAP_FAILED) { close(fd); return false; }
    r->cq_map = (p.features & IORING_FEAT_SINGLE_MMAP) ? r->sq_map
              : mmap(NULL, r->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    r->sqes_size = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (r->cq_map == MAP_FAILED || r->sqes == MAP_FAILED) { ring_unmap(r); close(fd); return false; }
    char *sqp = r->sq_map, *cqp = r->cq_map;
    r->sq_head = (proven_u32 *)(void *)(sqp + p.sq_off.head);
    r->sq_tail = (proven_u32 *)(void *)(sqp + p.sq_off.tail);
    r->sq_mask = (proven_u32 *)(void *)(sqp + p.sq_off.ring_mask);
    r->sq_array = (proven_u32 *)(void *)(sqp + p.sq_off.array);
    r->cq_head = (proven_u32 *)(void *)(cqp + p.cq_off.head);
    r->cq_tail = (proven_u32 *)(void *)(cqp + p.cq_off.tail);
    r->cq_mask = (proven_u32 *)(void *)(cqp + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)(void *)(cqp + p.cq_off.cqes);
    /* Entry i of the array names submission slot i, for good: slots are used in order. */
    for (proven_u32 i = 0; i < p.sq_entries; ++i) r->sq_array[i] = i;
    /* What the kernel must have: reports never dropped, entries read at submission, and a wait
     * with a time limit. */
    const proven_u32 need = IORING_FEAT_NODROP | IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_EXT_ARG;
    if ((p.features & need) != need) { ring_unmap(r); close(fd); return false; }
    return true;
}

/* Submit what was written; with `wait`, also wait up to `ms` (negative: no limit) for a report. */
static int ring_submit(ring_t *r, bool wait, int ms) {
    unsigned flags = wait ? IORING_ENTER_GETEVENTS : 0;
    ring_getevents_arg_t arg;
    struct __kernel_timespec ts;
    const void *argp = NULL;
    size_t argsz = 0;
    if (wait && ms >= 0) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long long)(ms % 1000) * 1000000;
        memset(&arg, 0, sizeof arg);
        arg.ts = (proven_u64)(proven_uintptr_t)&ts;
        flags |= IORING_ENTER_EXT_ARG;
        argp = &arg;
        argsz = sizeof arg;
    }
    for (;;) {
        const int n = ring_enter(r->fd, r->to_submit, wait && ms != 0 ? 1u : 0u, flags, argp, argsz);
        if (n >= 0) { r->to_submit -= (proven_u32)n > r->to_submit ? r->to_submit : (proven_u32)n; return 0; }
        if (errno == ETIME || errno == EINTR) return 0;
        if (errno == EBUSY || errno == EAGAIN) return 0;     /* reports to take first: the caller reaps */
        return -errno;
    }
}

/* A free submission entry, zeroed; written ones are submitted first when the queue is full. */
static struct io_uring_sqe *ring_sqe(ring_t *r) {
    for (int tries = 0; tries < 2; ++tries) {
        const proven_u32 tail = *r->sq_tail;
        const proven_u32 head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
        if (tail - head < r->sq_entries) {
            struct io_uring_sqe *e = &r->sqes[tail & *r->sq_mask];
            memset(e, 0, sizeof *e);
            return e;
        }
        (void)ring_submit(r, false, 0);
    }
    return NULL;
}

static void ring_push(ring_t *r) {
    __atomic_store_n(r->sq_tail, *r->sq_tail + 1, __ATOMIC_RELEASE);
    r->to_submit++;
}

static void ring_arm_wake(ring_t *r) {
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return;
    e->opcode = RING_OP_READ;
    e->fd = r->efd;
    e->addr = (proven_u64)(proven_uintptr_t)&r->wake_buf;
    e->len = sizeof r->wake_buf;
    e->user_data = RING_WAKE;
    ring_push(r);
}

static atomic_int g_ring_ok;                    /* 0 not asked, 1 yes, 2 no */
static bool g_ring_multi;                       /* multishot receives (6.0): written before g_ring_ok */

bool proven_sys_net_port_available(void) {
    int v = atomic_load_explicit(&g_ring_ok, memory_order_acquire);
    if (v == 0) {
        bool ok = false;
        ring_t r;
        if (ring_open(&r, 8, 16)) {
            ring_probe_t *probe = calloc(1, sizeof *probe);
            if (probe && syscall(__NR_io_uring_register, r.fd, RING_REGISTER_PROBE, probe, 256) >= 0) {
                static const int need[] = { RING_OP_POLL_ADD, RING_OP_SENDMSG, RING_OP_RECVMSG, RING_OP_ACCEPT, RING_OP_ASYNC_CANCEL,
                                            RING_OP_READ, RING_OP_RECV, RING_OP_SOCKET /* 5.19: cancelling a socket's all */ };
                ok = true;
                for (size_t i = 0; i < sizeof need / sizeof need[0]; ++i)
                    if (need[i] > probe->last_op || !(probe->ops[need[i]].flags & IO_URING_OP_SUPPORTED)) ok = false;
                g_ring_multi = RING_OP_SEND_ZC <= probe->last_op && (probe->ops[RING_OP_SEND_ZC].flags & IO_URING_OP_SUPPORTED);
            }
            free(probe);
            ring_unmap(&r);
            close(r.fd);
        }
        v = ok ? 1 : 2;
        atomic_store_explicit(&g_ring_ok, v, memory_order_release);
    }
    return v == 1;
}

bool proven_sys_net_port_preferred(void) { return proven_sys_net_port_available() && g_ring_multi; }

proven_sys_net_result_t proven_sys_net_port_open(proven_uintptr_t *out) {
    if (!proven_sys_net_port_available()) return PROVEN_SYS_NET_UNSUPPORTED;
    ring_t *r = malloc(sizeof *r);
    if (!r) return PROVEN_SYS_NET_LIMIT;
    if (!ring_open(r, 1024, 8192)) { free(r); return PROVEN_SYS_NET_UNSUPPORTED; }
    r->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (r->efd < 0) { ring_unmap(r); close(r->fd); free(r); return PROVEN_SYS_NET_LIMIT; }
    ring_arm_wake(r);
    *out = (proven_uintptr_t)r;
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_port_close(proven_uintptr_t port) {
    ring_t *r = (ring_t *)port;
    if (!r) return;
    ring_unmap(r);
    close(r->fd);                               /* the kernel cancels what is left */
    close(r->efd);
    /* The pool after the ring: until the ring is gone the kernel may still write into it. */
    if (r->pool_ring) munmap(r->pool_ring, r->pool_ring_size);
    if (r->pool_mem) munmap(r->pool_mem, r->pool_mem_size);
    free(r->pool_len);
    free(r->pool_next);
    free(r);
}

/* A buffer back into the buffer ring, for the kernel to fill again. */
static void pool_give(ring_t *r, proven_u16 bid) {
    ring_buf_t *b = &r->pool_ring[r->pool_tail & (r->pool_count - 1u)];
    b->addr = (proven_u64)(proven_uintptr_t)(r->pool_mem + (size_t)bid * r->pool_bytes);
    b->len = r->pool_bytes;
    b->bid = bid;
    r->pool_tail++;
    __atomic_store_n(&r->pool_ring[0].resv, r->pool_tail, __ATOMIC_RELEASE);
}

bool proven_sys_net_port_pool(proven_uintptr_t port, proven_size_t count, proven_size_t bytes) {
    ring_t *r = (ring_t *)port;
    if (r->pool_tried) return r->pool_ok;
    r->pool_tried = true;
    if (!g_ring_multi || count == 0 || count > 32768u || (count & (count - 1u)) != 0 || bytes == 0 || bytes > 65536u) return false;
    const size_t page = 4096;
    r->pool_count = (proven_u32)count;
    r->pool_bytes = (proven_u32)bytes;
    r->pool_ring_size = (count * sizeof(ring_buf_t) + page - 1) / page * page;
    r->pool_mem_size = (count * bytes + page - 1) / page * page;
    r->pool_ring = mmap(NULL, r->pool_ring_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    r->pool_mem = mmap(NULL, r->pool_mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    r->pool_len = calloc(count, sizeof *r->pool_len);
    r->pool_next = calloc(count, sizeof *r->pool_next);
    if (r->pool_ring == MAP_FAILED) r->pool_ring = NULL;
    if (r->pool_mem == MAP_FAILED) r->pool_mem = NULL;
    ring_buf_reg_t reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr = (proven_u64)(proven_uintptr_t)r->pool_ring;
    reg.ring_entries = (proven_u32)count;
    reg.bgid = RING_POOL_GROUP;
    if (!r->pool_ring || !r->pool_mem || !r->pool_len || !r->pool_next ||
        syscall(__NR_io_uring_register, r->fd, RING_REGISTER_PBUF_RING, &reg, 1) < 0) {
        if (r->pool_ring) munmap(r->pool_ring, r->pool_ring_size);
        if (r->pool_mem) munmap(r->pool_mem, r->pool_mem_size);
        free(r->pool_len);
        free(r->pool_next);
        r->pool_ring = NULL; r->pool_mem = NULL; r->pool_len = NULL; r->pool_next = NULL;
        return false;
    }
    for (proven_u32 i = 0; i < r->pool_count; ++i) pool_give(r, (proven_u16)i);
    r->pool_ok = true;
    return true;
}

bool proven_sys_net_port_pool_free(proven_uintptr_t port) {
    const ring_t *r = (const ring_t *)port;
    return r->pool_ok && r->pool_held < r->pool_count;
}

proven_size_t proven_sys_net_port_pool_held(proven_uintptr_t port) { return ((const ring_t *)port)->pool_held; }

proven_sys_net_result_t proven_sys_net_port_bind(proven_uintptr_t port, proven_sys_socket_t sock, bool allow_skip, bool *skip) {
    (void)port; (void)sock;
    /* Nothing to bind: every entry names its port. A send on such a socket is tried on the socket
     * first and finishes there when it can, reported by nobody - as a WSASend does on Windows
     * with the skip mode (proven_sys_net_ov_send). */
    *skip = allow_skip;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_port_post(proven_uintptr_t port) {
    const ring_t *r = (const ring_t *)port;
    const proven_u64 one = 1;
    for (;;) {
        if (write(r->efd, &one, sizeof one) == (ssize_t)sizeof one) return PROVEN_SYS_NET_OK;
        if (errno == EINTR) continue;
        return errno == EAGAIN ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_LIMIT;     /* the counter is full: a wake is due anyway */
    }
}

/* A report of a multishot receive: a filled buffer joins the end of its socket's list; the last
 * report says how the receive ended. -EINVAL there is a kernel without multishot receives after
 * all: the pool is not offered again. */
static proven_sys_net_done_t ring_multi_report(ring_t *r, proven_sys_net_ov_t *ov, int res, proven_u32 flags) {
    proven_u64 w = ov->w;
    if (flags & RING_CQE_F_BUFFER) {
        const proven_u16 bid = (proven_u16)(flags >> RING_CQE_BUFFER_SHIFT);
        if (res > 0) {
            r->pool_held++;
            r->pool_len[bid] = (proven_u32)res;
            if (w & MS_HELD) {
                r->pool_next[MS_TAIL(w)] = bid;
                w = (w & ~MS_LIST_MASK) | MS_LIST(MS_HEAD(w), bid, MS_OFF(w));
            } else {
                w = (w & ~MS_LIST_MASK) | MS_LIST(bid, bid, 0) | MS_HELD;
            }
        } else {
            pool_give(r, bid);
        }
    }
    const bool more = (flags & RING_CQE_F_MORE) != 0;
    if (!more) {
        proven_sys_net_result_t why = res > 0 ? PROVEN_SYS_NET_OK : res == 0 ? PROVEN_SYS_NET_EOF : res == -ENOBUFS ? PROVEN_SYS_NET_LIMIT
                                    : res == -ECANCELED ? PROVEN_SYS_NET_CANCELLED : res == -EINVAL ? PROVEN_SYS_NET_UNSUPPORTED : net_reason(-res);
        if (why == PROVEN_SYS_NET_UNSUPPORTED) r->pool_ok = false;
        w = (w & ~(MS_ARMED | ((proven_u64)0xffu << 56))) | MS_ENDED | ((proven_u64)(proven_u8)why << 56);
    }
    ov->w = w;
    return (proven_sys_net_done_t){ .ov = ov, .bytes = res > 0 ? (proven_u32)res : 0, .failed = false, .more = more };
}

/* Take up to `cap` reports from the completion queue, without a system call. */
static proven_size_t ring_reap(ring_t *r, proven_sys_net_done_t *out, proven_size_t cap) {
    proven_size_t n = 0;
    proven_u32 head = *r->cq_head;
    const proven_u32 tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
    bool rearm = false;
    while (head != tail && n < cap) {
        const struct io_uring_cqe *c = &r->cqes[head & *r->cq_mask];
        const proven_u64 ud = c->user_data;
        const int res = c->res;
        head++;
        if (ud == RING_IGNORE) continue;
        if (ud == RING_WAKE) {
            rearm = true;
            out[n++] = (proven_sys_net_done_t){ .ov = NULL, .bytes = 0, .failed = false };
            continue;
        }
        if (ud & RING_MULTI) {
            out[n++] = ring_multi_report(r, (proven_sys_net_ov_t *)(proven_uintptr_t)(ud & ~(proven_u64)3u), res, c->flags);
            continue;
        }
        proven_sys_net_ov_t *ov = (proven_sys_net_ov_t *)(proven_uintptr_t)ud;
        int got = res;
        if (RK_KIND(ov) == RK_MSG && res >= 0 && (RK_HDR(ov)->msg_flags & MSG_TRUNC)) got = RING_TRUNCATED;
        ov->w = ((proven_u64)(proven_u32)got << 32) | RK_KIND(ov);
        const bool failed = got < 0;
        out[n++] = (proven_sys_net_done_t){ .ov = ov, .bytes = failed || RK_KIND(ov) == RK_POLL ? 0 : (proven_u32)got, .failed = failed };
    }
    __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    if (rearm) ring_arm_wake(r);
    return n;
}

proven_sys_net_result_t proven_sys_net_port_wait(proven_uintptr_t port, proven_sys_net_done_t *out, proven_size_t cap,
                                                 int timeout_ms, proven_size_t *count) {
    ring_t *r = (ring_t *)port;
    *count = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    proven_size_t n = ring_reap(r, out, cap);
    /* Entered with GETEVENTS when nothing was there, waiting or not, so that a look that does not
     * wait still asks the kernel for what has finished. With reports in hand the kernel is
     * entered only to submit. */
    if ((n == 0 || r->to_submit > 0) && ring_submit(r, true, n == 0 ? timeout_ms : 0) < 0) return PROVEN_SYS_NET_ERROR;
    if (n < cap) n += ring_reap(r, out + n, cap - n);
    *count = n;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_ov_result(proven_sys_socket_t sock, proven_sys_net_ov_t *ov) {
    (void)sock;
    const int res = (int)(proven_u32)(ov->w >> 32);
    if (res >= 0) return PROVEN_SYS_NET_OK;
    if (res == RING_TRUNCATED) return PROVEN_SYS_NET_TRUNCATED;
    if (res == -ECANCELED) return PROVEN_SYS_NET_CANCELLED;
    return net_reason(-res);
}

void proven_sys_net_ov_cancel(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov) {
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return;
    e->opcode = RING_OP_ASYNC_CANCEL;
    e->user_data = RING_IGNORE;
    if (ov) {
        e->addr = (proven_u64)(proven_uintptr_t)ov;
    } else {
        e->fd = (int)sock;
        e->rw_flags = (int)(IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL);
    }
    ring_push(r);
    /* Submitted now, not at the next wait: the caller may close the socket as soon as this
     * returns (closing one does not cancel what the ring holds of it, as it does on Windows), and
     * a cancel by descriptor must find the descriptor still open. */
    (void)ring_submit(r, false, 0);
}

void proven_sys_net_port_cancel_all(proven_uintptr_t port) {
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return;
    e->opcode = RING_OP_ASYNC_CANCEL;
    e->user_data = RING_IGNORE;
    e->rw_flags = (int)IORING_ASYNC_CANCEL_ANY;
    ring_push(r);
}

static void ov_start(proven_sys_net_ov_t *ov, proven_uintptr_t kind) {
    memset(ov, 0, sizeof *ov);
    ov->w = kind;
}

proven_sys_net_result_t proven_sys_net_ov_recv(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, void *buf, proven_size_t cap,
                                               bool skip, proven_u32 *bytes) {
    (void)skip;
    *bytes = 0;
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return PROVEN_SYS_NET_LIMIT;
    e->fd = (int)sock;
    e->user_data = (proven_u64)(proven_uintptr_t)ov;
    if (cap == 0) {                             /* done when data has arrived, nothing taken */
        ov_start(ov, RK_POLL);
        e->opcode = RING_OP_POLL_ADD;
        e->rw_flags = POLLIN;
    } else {
        ov_start(ov, RK_IO);
        e->opcode = RING_OP_RECV;
        e->addr = (proven_u64)(proven_uintptr_t)buf;
        e->len = (proven_u32)clamp_len(cap);
    }
    ring_push(r);
    return PROVEN_SYS_NET_IN_PROGRESS;
}

proven_sys_net_result_t proven_sys_net_ov_recv_multi(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov) {
    ring_t *r = (ring_t *)port;
    if (!r->pool_ok) return PROVEN_SYS_NET_UNSUPPORTED;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return PROVEN_SYS_NET_LIMIT;
    e->opcode = RING_OP_RECV;
    e->fd = (int)sock;
    e->ioprio = RING_RECV_MULTISHOT;
    e->flags = RING_SQE_BUFFER_SELECT;
    e->buf_index = RING_POOL_GROUP;             /* the buffer group, in the same place */
    e->user_data = (proven_u64)(proven_uintptr_t)ov | RING_MULTI;
    ov->w = (ov->w & (MS_LIST_MASK | MS_HELD)) | MS_ARMED;     /* what it held stays, ahead of what comes */
    ring_push(r);
    return PROVEN_SYS_NET_IN_PROGRESS;
}

proven_sys_net_result_t proven_sys_net_ov_read(proven_uintptr_t port, proven_sys_net_ov_t *ov, void *buf, proven_size_t cap, proven_size_t *n) {
    ring_t *r = (ring_t *)port;
    proven_u64 w = ov->w;
    proven_size_t total = 0;
    while (total < cap && (w & MS_HELD)) {
        const proven_u16 bid = MS_HEAD(w);
        proven_u32 off = MS_OFF(w);
        proven_size_t k = r->pool_len[bid] - off;
        if (k > cap - total) k = cap - total;
        memcpy((proven_byte_t *)buf + total, r->pool_mem + (size_t)bid * r->pool_bytes + off, k);
        total += k;
        off += (proven_u32)k;
        if (off < r->pool_len[bid]) { w = (w & ~MS_LIST_MASK) | MS_LIST(bid, MS_TAIL(w), off); break; }
        /* This buffer is taken: back to the pool, and the list starts at the next. */
        if (bid == MS_TAIL(w)) w &= ~(MS_LIST_MASK | MS_HELD);
        else w = (w & ~MS_LIST_MASK) | MS_LIST(r->pool_next[bid], MS_TAIL(w), 0);
        r->pool_held--;
        pool_give(r, bid);
    }
    ov->w = w;
    *n = total;
    if (total > 0) return PROVEN_SYS_NET_OK;
    if (w & MS_ARMED) return PROVEN_SYS_NET_WOULD_BLOCK;
    if (!(w & MS_ENDED)) return PROVEN_SYS_NET_NOT_FOUND;
    const proven_sys_net_result_t why = MS_REASON(w);
    if (why == PROVEN_SYS_NET_OK || why == PROVEN_SYS_NET_LIMIT || why == PROVEN_SYS_NET_CANCELLED || why == PROVEN_SYS_NET_UNSUPPORTED) return PROVEN_SYS_NET_NOT_FOUND;
    return why;
}

unsigned proven_sys_net_ov_held(proven_uintptr_t port, const proven_sys_net_ov_t *ov) {
    const ring_t *r = (const ring_t *)port;
    const proven_u64 w = ov->w;
    if (!(w & MS_HELD)) return 0;
    unsigned k = 1;
    for (proven_u16 b = MS_HEAD(w); b != MS_TAIL(w) && k < 15; b = r->pool_next[b]) ++k;
    return k;
}

void proven_sys_net_ov_cancel_multi(proven_uintptr_t port, proven_sys_net_ov_t *ov) {
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return;
    e->opcode = RING_OP_ASYNC_CANCEL;
    e->user_data = RING_IGNORE;
    e->addr = (proven_u64)(proven_uintptr_t)ov | RING_MULTI;
    ring_push(r);
}

void proven_sys_net_ov_drop(proven_uintptr_t port, proven_sys_net_ov_t *ov) {
    ring_t *r = (ring_t *)port;
    proven_u64 w = ov->w;
    while (w & MS_HELD) {
        const proven_u16 bid = MS_HEAD(w);
        if (bid == MS_TAIL(w)) w &= ~(MS_LIST_MASK | MS_HELD);
        else w = (w & ~MS_LIST_MASK) | MS_LIST(r->pool_next[bid], MS_TAIL(w), 0);
        r->pool_held--;
        pool_give(r, bid);
    }
    ov->w = w & MS_ARMED;
}

proven_sys_net_result_t proven_sys_net_ov_send(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, proven_sys_net_keep_t *keep,
                                               const proven_sys_net_vec_t *vec, proven_size_t count, bool skip, proven_u32 *bytes) {
    *bytes = 0;
    ring_t *r = (ring_t *)port;
    ring_keep_t *k = (ring_keep_t *)(void *)keep;
    if (count > PROVEN_SYS_NET_VEC_MAX) count = PROVEN_SYS_NET_VEC_MAX;
    memset(&k->m, 0, sizeof k->m);
    for (proven_size_t i = 0; i < count; ++i) { k->iov[i].iov_base = (void *)(proven_uintptr_t)vec[i].ptr; k->iov[i].iov_len = vec[i].len; }
    k->m.msg_iov = k->iov;
    k->m.msg_iovlen = count;
    /* With `skip`, what the socket takes now is sent now, as on Windows: the loop and the drivers
     * above it count on a send finishing at once when it can - always handing it to the ring let
     * an HTTP/2 server over TLS take every request of a client that read nothing (RFC-0029). The
     * ring is asked only when the socket is full. */
    if (skip) {
        const ssize_t n = sendmsg((int)sock, &k->m, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) { *bytes = (proven_u32)n; return PROVEN_SYS_NET_OK; }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return net_reason(errno);
    }
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return PROVEN_SYS_NET_LIMIT;
    ov_start(ov, RK_IO);
    e->opcode = RING_OP_SENDMSG;
    e->fd = (int)sock;
    e->addr = (proven_u64)(proven_uintptr_t)&k->m;
    e->len = 1;
    e->rw_flags = MSG_NOSIGNAL;
    e->user_data = (proven_u64)(proven_uintptr_t)ov;
    ring_push(r);
    return PROVEN_SYS_NET_IN_PROGRESS;
}

proven_sys_net_result_t proven_sys_net_ov_accept(proven_uintptr_t port, proven_sys_socket_t listener, proven_sys_net_family_t family, proven_sys_net_ov_t *ov,
                                                 void *area, bool skip, proven_sys_socket_t *accepted) {
    (void)family; (void)area; (void)skip;
    *accepted = PROVEN_SYS_SOCKET_INVALID;      /* the report's `bytes` is the new socket */
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return PROVEN_SYS_NET_LIMIT;
    ov_start(ov, RK_IO);
    e->opcode = RING_OP_ACCEPT;
    e->fd = (int)listener;
    e->rw_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    e->user_data = (proven_u64)(proven_uintptr_t)ov;
    ring_push(r);
    return PROVEN_SYS_NET_IN_PROGRESS;
}

proven_sys_net_result_t proven_sys_net_ov_accept_done(proven_sys_socket_t listener, proven_sys_socket_t accepted) {
    (void)listener; (void)accepted;
    return PROVEN_SYS_NET_OK;                   /* accept4's flags made it non-blocking and close-on-exec */
}

proven_sys_net_result_t proven_sys_net_ov_connect_socket(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out) {
    const int af = native_family(addr->family);
    if (af < 0) return PROVEN_SYS_NET_ADDR_INVALID;
    const proven_native_socket_t s = native_open(af, SOCK_STREAM);
    if (s == NATIVE_INVALID) return net_fail();
    *out = (proven_sys_socket_t)s;
    return PROVEN_SYS_NET_OK;
}

/* A connect started now; what is not finished is waited for as writable, through the ring. */
proven_sys_net_result_t proven_sys_net_ov_connect(proven_uintptr_t port, proven_sys_socket_t sock, const proven_sys_net_addr_t *addr, proven_sys_net_ov_t *ov,
                                                  bool skip) {
    (void)skip;
    struct sockaddr_storage ss;
    const proven_native_len_t len = to_native(addr, &ss);
    if (len == 0) return PROVEN_SYS_NET_ADDR_INVALID;
    for (;;) {
        if (connect((proven_native_socket_t)sock, (const struct sockaddr *)&ss, len) == 0) return PROVEN_SYS_NET_OK;
        if (errno == EINTR) continue;
        break;
    }
    if (errno != EINPROGRESS) return net_reason(errno);
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return PROVEN_SYS_NET_LIMIT;
    ov_start(ov, RK_POLL);
    e->opcode = RING_OP_POLL_ADD;
    e->fd = (int)sock;
    e->rw_flags = POLLOUT;
    e->user_data = (proven_u64)(proven_uintptr_t)ov;
    ring_push(r);
    return PROVEN_SYS_NET_IN_PROGRESS;
}

proven_sys_net_result_t proven_sys_net_ov_connect_done(proven_sys_socket_t sock) {
    int err = 0;
    socklen_t len = (socklen_t)sizeof err;
    if (getsockopt((proven_native_socket_t)sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return net_fail();
    return err == 0 ? PROVEN_SYS_NET_OK : net_reason(err);
}

proven_sys_net_result_t proven_sys_net_ov_recv_msg(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, proven_sys_net_msg_t *msg,
                                                   void *buf, proven_size_t cap, bool skip, proven_u32 *bytes) {
    (void)skip;
    *bytes = 0;
    ring_msg_t *m = (ring_msg_t *)(void *)msg;
    memset(m, 0, sizeof *m);
    m->iov.iov_base = buf;
    m->iov.iov_len = (size_t)clamp_len(cap);
    m->m.msg_name = &m->from;
    m->m.msg_namelen = (socklen_t)sizeof m->from;
    m->m.msg_iov = &m->iov;
    m->m.msg_iovlen = 1;
    m->m.msg_control = m->ctl.b;
    m->m.msg_controllen = sizeof m->ctl.b;
    ring_t *r = (ring_t *)port;
    struct io_uring_sqe *e = ring_sqe(r);
    if (!e) return PROVEN_SYS_NET_LIMIT;
    ov_start(ov, RK_MSG);
    ov->w |= (proven_u64)(proven_uintptr_t)&m->m;
    e->opcode = RING_OP_RECVMSG;
    e->fd = (int)sock;
    e->addr = (proven_u64)(proven_uintptr_t)&m->m;
    e->len = 1;
    e->user_data = (proven_u64)(proven_uintptr_t)ov;
    ring_push(r);
    return PROVEN_SYS_NET_IN_PROGRESS;
}

void proven_sys_net_ov_msg_done(const proven_sys_net_msg_t *msg, proven_u32 bytes, proven_size_t *seg, proven_u8 *ecn, proven_sys_net_addr_t *from) {
    const ring_msg_t *m = (const ring_msg_t *)(const void *)msg;
    *seg = 0;
    if (ecn) *ecn = 0;
    if (from) from_native(&m->from, m->m.msg_namelen, from);
    struct msghdr copy = m->m;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&copy); c; c = CMSG_NXTHDR(&copy, c)) {
        const int mark = ecn_of(c);
        if (mark >= 0) { if (ecn) *ecn = (proven_u8)mark; continue; }
#if defined(UDP_GRO)
        if (c->cmsg_level != IPPROTO_UDP || c->cmsg_type != UDP_GRO) continue;
        int size = 0;
        memcpy(&size, CMSG_DATA(c), sizeof size);
        if (size > 0 && (proven_u32)size < bytes) *seg = (proven_size_t)size;
#endif
    }
#if !defined(UDP_GRO)
    (void)bytes;
#endif
}
#endif /* PROVEN_SYS_RING */

#else
/* A translation unit must not be empty. */
typedef int proven_sys_net_unused_t;
#endif
