// src/pal/posix/pal_posix.c - POSIX: output and paths are UTF-8 bytes as they are.

#define _POSIX_C_SOURCE 200809L

#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len) {
    FILE *f = (out == RP_OUT_STDERR) ? stderr : stdout;
    if (utf8 == NULL && len != 0) return PROVEN_ERR_INVALID_ARG;
    if (len != 0 && fwrite(utf8, 1, len, f) != len) return PROVEN_ERR_IO;
    if (fflush(f) != 0) return PROVEN_ERR_IO;
    return PROVEN_OK;
}

proven_err_t rp_pal_puts(rp_out_t out, const char *utf8) {
    if (utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    return rp_pal_write(out, (const uint8_t *)utf8, strlen(utf8));
}

rp_fskind_t rp_pal_stat(proven_allocator_t alloc, const char *path_utf8, uint64_t *size) {
    (void)alloc;
    struct stat st;
    if (path_utf8 == NULL || lstat(path_utf8, &st) != 0) return RP_FS_NONE;
    if (S_ISLNK(st.st_mode)) return RP_FS_LINK;
    if (S_ISDIR(st.st_mode)) return RP_FS_DIR;
    if (!S_ISREG(st.st_mode)) return RP_FS_OTHER;
    if (size) *size = (uint64_t)st.st_size;
    return RP_FS_FILE;
}

proven_err_t rp_pal_read_line(proven_allocator_t alloc, size_t max_bytes, char **line) {
    if (line == NULL || max_bytes == 0) return PROVEN_ERR_INVALID_ARG;
    *line = NULL;
    size_t cap = 128, n = 0;
    char *buf = rp_mem_alloc(alloc, cap, 1);
    if (buf == NULL) return PROVEN_ERR_NOMEM;
    int ch = EOF;
    while ((ch = getchar()) != EOF && ch != '\n') {
        if (n + 2 > cap) {
            if (cap >= max_bytes + 2) {
                rp_mem_free(alloc, buf);
                return PROVEN_ERR_OUT_OF_BOUNDS;
            }
            char *more = rp_mem_alloc(alloc, cap * 2, 1);
            if (more == NULL) {
                rp_mem_free(alloc, buf);
                return PROVEN_ERR_NOMEM;
            }
            memcpy(more, buf, n);
            rp_mem_free(alloc, buf);
            buf = more;
            cap *= 2;
        }
        buf[n++] = (char)ch;
    }
    if (ch == EOF && n == 0) {
        rp_mem_free(alloc, buf);
        return PROVEN_ERR_NOT_FOUND;
    }
    if (n && buf[n - 1] == '\r') --n;
    buf[n] = '\0';
    if (n > max_bytes) {
        rp_mem_free(alloc, buf);
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (rp_utf8_validate((const uint8_t *)buf, n).err != PROVEN_OK) {
        rp_mem_free(alloc, buf);
        return PROVEN_ERR_INVALID_ENCODING;
    }
    *line = buf;
    return PROVEN_OK;
}

bool rp_pal_same_file(proven_allocator_t alloc, const char *a_utf8, const char *b_utf8) {
    (void)alloc;
    struct stat a, b;
    if (a_utf8 == NULL || b_utf8 == NULL || stat(a_utf8, &a) != 0 || stat(b_utf8, &b) != 0) return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

char *rp_pal_getenv(proven_allocator_t alloc, const char *name) {
    const char *v = getenv(name);
    if (v == NULL) return NULL;
    size_t n = strlen(v);
    char *c = rp_mem_alloc(alloc, n + 1, 1);
    if (c) memcpy(c, v, n + 1);
    return c;
}

// ---- network ----------------------------------------------------------------------------------

struct rp_sock {
    proven_allocator_t alloc;
    int                fd;
};

int64_t rp_pal_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static proven_err_t wait_fd(int fd, short events, int timeout_ms) {
    struct pollfd p = { fd, events, 0 };
    int r;
    do {
        r = poll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r == 0) return PROVEN_ERR_AGAIN;
    if (r < 0) return PROVEN_ERR_IO;
    return PROVEN_OK;
}

proven_err_t rp_pal_tcp_connect(proven_allocator_t alloc, const char *host, uint16_t port, int timeout_ms, rp_sock_t **out) {
    char service[8];
    snprintf(service, sizeof service, "%u", (unsigned)port);
    struct addrinfo hints = { 0 }, *list = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, service, &hints, &list) != 0 || list == NULL) return PROVEN_ERR_NOT_FOUND;
    proven_err_t err = PROVEN_ERR_IO;
    int64_t deadline = rp_pal_now_ms() + timeout_ms;
    for (struct addrinfo *a = list; a && err != PROVEN_OK; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            err = wait_fd(fd, POLLOUT, (int)(deadline - rp_pal_now_ms()));
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (err == PROVEN_OK && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0)) err = PROVEN_ERR_IO;
        } else {
            err = r == 0 ? PROVEN_OK : PROVEN_ERR_IO;
        }
        if (err == PROVEN_OK) {
            rp_sock_t *s = rp_mem_alloc(alloc, 1, sizeof *s);
            if (s == NULL) {
                close(fd);
                err = PROVEN_ERR_NOMEM;
                break;
            }
            s->alloc = alloc;
            s->fd = fd;
            *out = s;
        } else {
            close(fd);
        }
    }
    freeaddrinfo(list);
    return err;
}

proven_err_t rp_pal_tcp_send(rp_sock_t *s, const uint8_t *data, size_t len, int timeout_ms) {
    int64_t deadline = rp_pal_now_ms() + timeout_ms;
    while (len) {
        proven_err_t err = wait_fd(s->fd, POLLOUT, (int)(deadline - rp_pal_now_ms()));
        if (err != PROVEN_OK) return err;
        ssize_t n = send(s->fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return PROVEN_ERR_IO;
        }
        data += n;
        len -= (size_t)n;
    }
    return PROVEN_OK;
}

proven_err_t rp_pal_tcp_recv(rp_sock_t *s, uint8_t *buf, size_t cap, size_t *got, int timeout_ms) {
    *got = 0;
    for (;;) {
        proven_err_t err = wait_fd(s->fd, POLLIN, timeout_ms);
        if (err != PROVEN_OK) return err;
        ssize_t n = recv(s->fd, buf, cap, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return PROVEN_ERR_IO;
        }
        *got = (size_t)n;
        return PROVEN_OK;
    }
}

void rp_pal_tcp_close(rp_sock_t *s) {
    if (s == NULL) return;
    close(s->fd);
    rp_mem_free(s->alloc, s);
}

proven_err_t rp_pal_system_roots(proven_allocator_t alloc, void (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx) {
    static const char *const paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",          // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",            // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                      // openSUSE
        "/etc/ssl/cert.pem",                           // Alpine, macOS, the BSDs
    };
    const char *env = getenv("SSL_CERT_FILE");
    for (size_t i = env && *env ? 0 : 1; i <= sizeof paths / sizeof paths[0]; ++i) {
        const char *path = i == 0 ? env : paths[i - 1];
        uint8_t *data;
        size_t len;
        if (rp_pal_read_file(alloc, path, 16u << 20, &data, &len) != PROVEN_OK) {
            if (i == 0) return PROVEN_ERR_NOT_FOUND;        // an explicit file that is not there
            continue;
        }
        sink(ctx, data, len);
        rp_mem_free(alloc, data);
        return PROVEN_OK;
    }
    return PROVEN_ERR_NOT_FOUND;
}

// ---- parallel work (RFC-0013 E2) ----------------------------------------------------------------

typedef struct {
    void (*fn)(void *ctx, size_t i);
    void *ctx;
    size_t count;
    _Atomic size_t next;
} rp_work_t;

static void rp_work_run(rp_work_t *w) {
    for (;;) {
        size_t i = atomic_fetch_add(&w->next, 1);
        if (i >= w->count) return;
        w->fn(w->ctx, i);
    }
}

size_t rp_pal_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (size_t)n : 1;
}

static void *rp_work_thread(void *arg) {
    rp_work_run(arg);
    return NULL;
}

void rp_pal_parallel_for(size_t jobs, size_t count, void (*fn)(void *ctx, size_t i), void *ctx) {
    rp_work_t w = { .fn = fn, .ctx = ctx, .count = count };
    atomic_init(&w.next, 0);
    if (jobs > count) jobs = count;
    if (jobs > 64) jobs = 64;
    pthread_t t[64];
    size_t started = 0;
    for (size_t k = 1; k < jobs; ++k) {
        if (pthread_create(&t[started], NULL, rp_work_thread, &w) == 0) ++started;
    }
    rp_work_run(&w);        // this thread works too; with no helper thread it does everything
    for (size_t k = 0; k < started; ++k) pthread_join(t[k], NULL);
}

// ---- mapped files (RFC-0013 E1) ------------------------------------------------------------------

struct rp_outmap {
    void  *p;
    size_t n;
    int    fd;
    char  *tmp, *path;
};

static void outmap_free(proven_allocator_t alloc, rp_outmap_t *m) {
    rp_mem_free(alloc, m->tmp);
    rp_mem_free(alloc, m->path);
    rp_mem_free(alloc, m);
}

proven_err_t rp_pal_outmap_create(proven_allocator_t alloc, const char *path_utf8, size_t len, uint8_t **data, rp_outmap_t **om) {
    if (path_utf8 == NULL || data == NULL || om == NULL || len == 0 || len > (size_t)INT64_MAX) return PROVEN_ERR_INVALID_ARG;
    *data = NULL;
    *om = NULL;
    size_t n = strlen(path_utf8);
    rp_outmap_t *m = rp_mem_alloc(alloc, 1, sizeof *m);
    if (m == NULL) return PROVEN_ERR_NOMEM;
    *m = (rp_outmap_t){ NULL, len, -1, rp_mem_alloc(alloc, n + 16, 1), rp_mem_alloc(alloc, n + 1, 1) };
    if (m->tmp == NULL || m->path == NULL) {
        outmap_free(alloc, m);
        return PROVEN_ERR_NOMEM;
    }
    snprintf(m->tmp, n + 16, "%s.rp-map", path_utf8);
    memcpy(m->path, path_utf8, n + 1);
    m->fd = open(m->tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (m->fd < 0) {
        outmap_free(alloc, m);
        return PROVEN_ERR_IO;
    }
    // Reserve the space now: a full disk met through the mapping would be a signal, not an error.
    if (posix_fallocate(m->fd, 0, (off_t)len) != 0 ||
        (m->p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0)) == MAP_FAILED) {
        m->p = NULL;
        rp_pal_outmap_discard(alloc, m);
        return PROVEN_ERR_IO;
    }
    *data = m->p;
    *om = m;
    return PROVEN_OK;
}

proven_err_t rp_pal_outmap_commit(proven_allocator_t alloc, rp_outmap_t *om) {
    if (om == NULL) return PROVEN_ERR_INVALID_ARG;
    proven_err_t err = PROVEN_OK;
    if (msync(om->p, om->n, MS_SYNC) != 0) err = PROVEN_ERR_IO;
    munmap(om->p, om->n);
    if (fsync(om->fd) != 0) err = PROVEN_ERR_IO;
    if (close(om->fd) != 0) err = PROVEN_ERR_IO;
    if (err == PROVEN_OK && rename(om->tmp, om->path) != 0) err = PROVEN_ERR_IO;
    if (err != PROVEN_OK) unlink(om->tmp);
    outmap_free(alloc, om);
    return err;
}

void rp_pal_outmap_discard(proven_allocator_t alloc, rp_outmap_t *om) {
    if (om == NULL) return;
    if (om->p) munmap(om->p, om->n);
    if (om->fd >= 0) close(om->fd);
    unlink(om->tmp);
    outmap_free(alloc, om);
}
