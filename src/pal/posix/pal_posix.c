// src/pal/posix/pal_posix.c - POSIX: output and paths are UTF-8 bytes as they are.

#define _POSIX_C_SOURCE 200809L

#include "rubrapack/mem.h"
#include "rubrapack/pal.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

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

proven_err_t rp_pal_read_file(proven_allocator_t alloc, const char *path_utf8, size_t max_bytes,
                              uint8_t **data, size_t *len) {
    if (path_utf8 == NULL || data == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    FILE *f = fopen(path_utf8, "rb");
    if (f == NULL) return PROVEN_ERR_NOT_FOUND;
    proven_err_t err = PROVEN_OK;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) err = PROVEN_ERR_IO;
    else if ((unsigned long)size > max_bytes) err = PROVEN_ERR_OUT_OF_BOUNDS;
    uint8_t *buf = NULL;
    if (err == PROVEN_OK) {
        buf = rp_mem_alloc(alloc, (size_t)size, 1);
        if (buf == NULL) err = PROVEN_ERR_NOMEM;
        else if (fread(buf, 1, (size_t)size, f) != (size_t)size) err = PROVEN_ERR_IO;
    }
    fclose(f);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, buf);
        return err;
    }
    *data = buf;
    *len = (size_t)size;
    return PROVEN_OK;
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

proven_err_t rp_pal_write_file_atomic(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data, size_t len) {
    if (path_utf8 == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    size_t n = strlen(path_utf8);
    char *tmp = rp_mem_alloc(alloc, n + 16, 1);
    if (tmp == NULL) return PROVEN_ERR_NOMEM;
    snprintf(tmp, n + 16, "%s.rp-tmp", path_utf8);
    FILE *f = fopen(tmp, "wb");
    proven_err_t err = PROVEN_OK;
    if (f == NULL) {
        err = PROVEN_ERR_IO;
    } else {
        if (len && fwrite(data, 1, len, f) != len) err = PROVEN_ERR_IO;
        if (fflush(f) != 0 || fsync(fileno(f)) != 0) err = PROVEN_ERR_IO;
        if (fclose(f) != 0) err = PROVEN_ERR_IO;
        if (err == PROVEN_OK && rename(tmp, path_utf8) != 0) err = PROVEN_ERR_IO;
        if (err != PROVEN_OK) remove(tmp);
    }
    rp_mem_free(alloc, tmp);
    return err;
}

proven_err_t rp_pal_list_dir(proven_allocator_t alloc, const char *path_utf8, char ***names, size_t *count) {
    if (path_utf8 == NULL || names == NULL || count == NULL) return PROVEN_ERR_INVALID_ARG;
    DIR *d = opendir(path_utf8);
    if (d == NULL) return PROVEN_ERR_NOT_FOUND;
    char **v = NULL;
    size_t n = 0, cap = 0;
    proven_err_t err = PROVEN_OK;
    struct dirent *e;
    while (err == PROVEN_OK && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t len = strlen(e->d_name);
        if (rp_utf8_validate((const uint8_t *)e->d_name, len).err != PROVEN_OK) {
            err = PROVEN_ERR_INVALID_ENCODING;
            break;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            char **nv = rp_mem_alloc(alloc, ncap, sizeof *nv);
            if (nv == NULL) {
                err = PROVEN_ERR_NOMEM;
                break;
            }
            if (n) memcpy(nv, v, n * sizeof *nv);
            rp_mem_free(alloc, v);
            v = nv;
            cap = ncap;
        }
        v[n] = rp_mem_alloc(alloc, len + 1, 1);
        if (v[n] == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        memcpy(v[n++], e->d_name, len + 1);
    }
    closedir(d);
    if (err != PROVEN_OK) {
        for (size_t k = 0; k < n; ++k) rp_mem_free(alloc, v[k]);
        rp_mem_free(alloc, v);
        return err;
    }
    *names = v;
    *count = n;
    return PROVEN_OK;
}

bool rp_pal_same_file(proven_allocator_t alloc, const char *a_utf8, const char *b_utf8) {
    (void)alloc;
    struct stat a, b;
    if (a_utf8 == NULL || b_utf8 == NULL || stat(a_utf8, &a) != 0 || stat(b_utf8, &b) != 0) return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

proven_err_t rp_pal_mkdir_new(proven_allocator_t alloc, const char *path_utf8) {
    (void)alloc;
    if (path_utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    if (mkdir(path_utf8, 0777) == 0) return PROVEN_OK;
    return errno == EEXIST ? PROVEN_ERR_BUSY : PROVEN_ERR_IO;
}

proven_err_t rp_pal_write_file_new(proven_allocator_t alloc, const char *path_utf8, const uint8_t *data, size_t len) {
    (void)alloc;
    if (path_utf8 == NULL || (data == NULL && len != 0)) return PROVEN_ERR_INVALID_ARG;
    // O_EXCL with O_CREAT fails on any existing name, a symbolic link included (never followed).
    int fd = open(path_utf8, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) return errno == EEXIST ? PROVEN_ERR_BUSY : PROVEN_ERR_IO;
    proven_err_t err = PROVEN_OK;
    for (size_t off = 0; err == PROVEN_OK && off < len;) {
        ssize_t w = write(fd, data + off, len - off > 0x40000000u ? 0x40000000u : len - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            err = PROVEN_ERR_IO;
        } else {
            off += (size_t)w;
        }
    }
    if (close(fd) != 0) err = PROVEN_ERR_IO;
    if (err != PROVEN_OK) unlink(path_utf8);
    return err;
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
