// src/pal/win32/pal_win32.c - Windows output: a console gets UTF-16 through WriteConsoleW,
// a pipe or file gets the UTF-8 bytes unchanged (RFC-0001 section 15.4).

#include "rubrapack/mem.h"
#include "rubrapack/pal.h"

#include <stdatomic.h>
#include "rubrapack/text.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

static proven_err_t write_bytes(HANDLE h, const uint8_t *p, size_t len) {
    while (len > 0) {
        DWORD chunk = (len > 0x10000u) ? 0x10000u : (DWORD)len;
        DWORD done = 0;
        if (!WriteFile(h, p, chunk, &done, NULL) || done == 0) return PROVEN_ERR_IO;
        p += done;
        len -= done;
    }
    return PROVEN_OK;
}

// Converts and writes in pieces that end on a UTF-8 sequence boundary, so no buffer larger
// than `wide` is needed and a surrogate pair is never split between two calls.
static proven_err_t write_console(HANDLE h, const uint8_t *p, size_t len) {
    enum { PIECE = 1024 };
    wchar_t wide[PIECE * 2];
    while (len > 0) {
        size_t take = (len > PIECE) ? PIECE : len;
        while (take < len && take > 0 && (p[take] & 0xC0u) == 0x80u) --take;
        if (take == 0) return PROVEN_ERR_INVALID_ENCODING;
        rp_text_result_t r = rp_utf8_to_utf16(p, take, (proven_u16 *)wide, PIECE * 2);
        if (r.err != PROVEN_OK) return r.err;
        DWORD left = (DWORD)r.units;
        const wchar_t *w = wide;
        while (left > 0) {
            DWORD done = 0;
            if (!WriteConsoleW(h, w, left, &done, NULL) || done == 0) return PROVEN_ERR_IO;
            w += done;
            left -= done;
        }
        p += take;
        len -= take;
    }
    return PROVEN_OK;
}

proven_err_t rp_pal_write(rp_out_t out, const uint8_t *utf8, size_t len) {
    if (utf8 == NULL && len != 0) return PROVEN_ERR_INVALID_ARG;
    HANDLE h = GetStdHandle(out == RP_OUT_STDERR ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL) return PROVEN_ERR_IO;
    DWORD mode;
    if (GetConsoleMode(h, &mode)) return write_console(h, utf8, len);
    return write_bytes(h, utf8, len);
}

proven_err_t rp_pal_puts(rp_out_t out, const char *utf8) {
    if (utf8 == NULL) return PROVEN_ERR_INVALID_ARG;
    return rp_pal_write(out, (const uint8_t *)utf8, strlen(utf8));
}

static proven_err_t wide_path(proven_allocator_t alloc, const char *path_utf8, proven_u16str_t *out);

proven_err_t rp_pal_read_line(proven_allocator_t alloc, size_t max_bytes, char **line) {
    if (line == NULL || max_bytes == 0) return PROVEN_ERR_INVALID_ARG;
    *line = NULL;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in == INVALID_HANDLE_VALUE || in == NULL) return PROVEN_ERR_NOT_FOUND;
    uint8_t *buf = NULL;
    size_t n = 0;
    bool eof = false;
    if (GetConsoleMode(in, &mode)) {
        // The console gives UTF-16: read up to the end of the line, then convert once.
        size_t cap = 256, w = 0;
        proven_u16 *wb = rp_mem_alloc(alloc, cap, sizeof *wb);
        if (wb == NULL) return PROVEN_ERR_NOMEM;
        for (;;) {
            if (w + 1 >= cap) {
                if (cap > max_bytes + 2) {
                    rp_mem_free(alloc, wb);
                    return PROVEN_ERR_OUT_OF_BOUNDS;
                }
                proven_u16 *more = rp_mem_alloc(alloc, cap * 2, sizeof *more);
                if (more == NULL) {
                    rp_mem_free(alloc, wb);
                    return PROVEN_ERR_NOMEM;
                }
                memcpy(more, wb, w * sizeof *wb);
                rp_mem_free(alloc, wb);
                wb = more;
                cap *= 2;
            }
            DWORD got = 0;
            if (!ReadConsoleW(in, wb + w, 1, &got, NULL) || got == 0) {
                eof = true;
                break;
            }
            if (wb[w] == 0x1A && w == 0) {      // Ctrl+Z, Enter: the end of the input
                eof = true;
                break;
            }
            if (wb[w] == L'\n') break;
            ++w;
        }
        if (w && wb[w - 1] == L'\r') --w;
        if (eof && w == 0) {
            rp_mem_free(alloc, wb);
            return PROVEN_ERR_NOT_FOUND;
        }
        rp_text_result_t need = rp_utf16_to_utf8(wb, w, NULL, 0);
        if (need.err != PROVEN_OK || need.units > max_bytes) {
            rp_mem_free(alloc, wb);
            return need.err != PROVEN_OK ? PROVEN_ERR_INVALID_ENCODING : PROVEN_ERR_OUT_OF_BOUNDS;
        }
        buf = rp_mem_alloc(alloc, need.units + 1, 1);
        if (buf == NULL) {
            rp_mem_free(alloc, wb);
            return PROVEN_ERR_NOMEM;
        }
        rp_text_result_t r = rp_utf16_to_utf8(wb, w, buf, need.units);
        rp_mem_free(alloc, wb);
        if (r.err != PROVEN_OK) {
            rp_mem_free(alloc, buf);
            return PROVEN_ERR_INVALID_ENCODING;
        }
        n = r.units;
    } else {
        // A pipe or a file: UTF-8 bytes, one at a time up to the line end.
        size_t cap = 128;
        buf = rp_mem_alloc(alloc, cap, 1);
        if (buf == NULL) return PROVEN_ERR_NOMEM;
        for (;;) {
            uint8_t ch;
            DWORD got = 0;
            if (!ReadFile(in, &ch, 1, &got, NULL) || got == 0) {
                eof = true;
                break;
            }
            if (ch == '\n') break;
            if (n + 2 > cap) {
                if (cap > max_bytes + 2) {
                    rp_mem_free(alloc, buf);
                    return PROVEN_ERR_OUT_OF_BOUNDS;
                }
                uint8_t *more = rp_mem_alloc(alloc, cap * 2, 1);
                if (more == NULL) {
                    rp_mem_free(alloc, buf);
                    return PROVEN_ERR_NOMEM;
                }
                memcpy(more, buf, n);
                rp_mem_free(alloc, buf);
                buf = more;
                cap *= 2;
            }
            buf[n++] = ch;
        }
        if (eof && n == 0) {
            rp_mem_free(alloc, buf);
            return PROVEN_ERR_NOT_FOUND;
        }
        if (n && buf[n - 1] == '\r') --n;
        if (n > max_bytes || rp_utf8_validate(buf, n).err != PROVEN_OK) {
            rp_mem_free(alloc, buf);
            return n > max_bytes ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_INVALID_ENCODING;
        }
    }
    buf[n] = 0;
    *line = (char *)buf;
    return PROVEN_OK;
}

static proven_err_t wide_path(proven_allocator_t alloc, const char *path_utf8, proven_u16str_t *out) {
    proven_u8str_view_t view = { .ptr = (const proven_byte_t *)path_utf8, .size = strlen(path_utf8) };
    rp_text_result_t t = rp_utf8_to_u16str(alloc, view, out);
    return t.err;
}

// A file path for the W calls, as proven's file calls make it (pal_fs.c): a full path of
// MAX_PATH or more characters gets the \\?\ (or \\?\UNC\) prefix, so long paths work here too.
static proven_err_t file_path(proven_allocator_t alloc, const char *path_utf8, proven_u16str_t *out) {
    proven_u16str_t w = { 0 };
    proven_err_t err = wide_path(alloc, path_utf8, &w);
    if (err != PROVEN_OK) return err;
    const wchar_t *in = (const wchar_t *)proven_u16str_as_ptr(&w);
    DWORD full = GetFullPathNameW(in, 0, NULL, NULL);
    if (full < MAX_PATH || wcsncmp(in, L"\\\\?\\", 4) == 0 || wcsncmp(in, L"\\\\.\\", 4) == 0) {
        *out = w;
        return PROVEN_OK;
    }
    wchar_t *buf = rp_mem_alloc(alloc, (size_t)full + 8, sizeof *buf);
    if (buf == NULL) {
        proven_u16str_destroy(alloc, &w);
        return PROVEN_ERR_NOMEM;
    }
    DWORD got = GetFullPathNameW(in, full, buf + 8, NULL);      // the full path, 8 units in
    proven_u16str_destroy(alloc, &w);
    if (got == 0 || got >= full) {
        rp_mem_free(alloc, buf);
        return PROVEN_ERR_IO;
    }
    wchar_t *at = buf + 8;
    if (at[0] == L'\\' && at[1] == L'\\') {       // \\server\share\... -> \\?\UNC\server\share\...
        at -= 6;
        memcpy(at, L"\\\\?\\UNC", 7 * sizeof *at);
    } else {                                    // C:\... -> \\?\C:\...
        at -= 4;
        memcpy(at, L"\\\\?\\", 4 * sizeof *at);
    }
    proven_result_u16str_t made = proven_u16str_create_from_view(alloc, (proven_u16str_view_t){ (const proven_u16 *)at, wcslen(at) });
    rp_mem_free(alloc, buf);
    if (made.err != PROVEN_OK) return made.err;
    *out = made.value;
    return PROVEN_OK;
}

rp_fskind_t rp_pal_stat(proven_allocator_t alloc, const char *path_utf8, uint64_t *size) {
    proven_u16str_t w = { 0 };
    if (path_utf8 == NULL || file_path(alloc, path_utf8, &w) != PROVEN_OK) return RP_FS_NONE;
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok = GetFileAttributesExW((const wchar_t *)proven_u16str_as_ptr(&w), GetFileExInfoStandard, &a);
    proven_u16str_destroy(alloc, &w);
    if (!ok) return RP_FS_NONE;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return RP_FS_LINK;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return RP_FS_DIR;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return RP_FS_FILE;
}

static bool file_id(proven_allocator_t alloc, const char *path_utf8, BY_HANDLE_FILE_INFORMATION *info) {
    proven_u16str_t w = { 0 };
    if (file_path(alloc, path_utf8, &w) != PROVEN_OK) return false;
    HANDLE h = CreateFileW((const wchar_t *)proven_u16str_as_ptr(&w), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    proven_u16str_destroy(alloc, &w);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = GetFileInformationByHandle(h, info);
    CloseHandle(h);
    return ok != 0;
}

bool rp_pal_same_file(proven_allocator_t alloc, const char *a_utf8, const char *b_utf8) {
    BY_HANDLE_FILE_INFORMATION a, b;
    if (a_utf8 == NULL || b_utf8 == NULL || !file_id(alloc, a_utf8, &a) || !file_id(alloc, b_utf8, &b)) return false;
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow;
}

char *rp_pal_getenv(proven_allocator_t alloc, const char *name) {
    proven_u16str_t w = { 0 };
    if (wide_path(alloc, name, &w) != PROVEN_OK) return NULL;
    const wchar_t *v = _wgetenv((const wchar_t *)proven_u16str_as_ptr(&w));
    proven_u16str_destroy(alloc, &w);
    if (v == NULL) return NULL;
    size_t n = wcslen(v);
    rp_text_result_t r = rp_utf16_to_utf8((const proven_u16 *)v, n, NULL, 0);
    if (r.err != PROVEN_OK) return NULL;
    char *c = rp_mem_alloc(alloc, r.units + 1, 1);
    if (c == NULL) return NULL;
    (void)rp_utf16_to_utf8((const proven_u16 *)v, n, (uint8_t *)c, r.units);
    c[r.units] = 0;
    return c;
}

// ---- network ----------------------------------------------------------------------------------

struct rp_sock {
    proven_allocator_t alloc;
    SOCKET             fd;
};

int64_t rp_pal_now_ms(void) { return (int64_t)GetTickCount64(); }

static bool winsock_ready(void) {
    static int state;           // 0 not tried, 1 ready, -1 failed
    if (state == 0) {
        WSADATA w;
        state = WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 1 : -1;
    }
    return state == 1;
}

static proven_err_t wait_sock(SOCKET fd, short events, int timeout_ms) {
    WSAPOLLFD p = { fd, events, 0 };
    int r = WSAPoll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (r == 0) return PROVEN_ERR_AGAIN;
    if (r < 0) return PROVEN_ERR_IO;
    return PROVEN_OK;
}

proven_err_t rp_pal_tcp_connect(proven_allocator_t alloc, const char *host, uint16_t port, int timeout_ms, rp_sock_t **out) {
    if (!winsock_ready()) return PROVEN_ERR_IO;
    proven_u16str_t wh = { 0 };
    if (wide_path(alloc, host, &wh) != PROVEN_OK) return PROVEN_ERR_NOT_FOUND;
    wchar_t service[8];
    swprintf(service, 8, L"%u", (unsigned)port);
    ADDRINFOW hints = { 0 }, *list = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    int gr = GetAddrInfoW((const wchar_t *)proven_u16str_as_ptr(&wh), service, &hints, &list);
    proven_u16str_destroy(alloc, &wh);
    if (gr != 0 || list == NULL) return PROVEN_ERR_NOT_FOUND;
    proven_err_t err = PROVEN_ERR_IO;
    int64_t deadline = rp_pal_now_ms() + timeout_ms;
    for (ADDRINFOW *a = list; a && err != PROVEN_OK; a = a->ai_next) {
        SOCKET fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(fd, FIONBIO, &nb);
        int r = connect(fd, a->ai_addr, (int)a->ai_addrlen);
        if (r != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            err = wait_sock(fd, POLLWRNORM, (int)(deadline - rp_pal_now_ms()));
            int soerr = 0, sl = sizeof soerr;
            if (err == PROVEN_OK && (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) != 0 || soerr != 0)) err = PROVEN_ERR_IO;
        } else {
            err = r == 0 ? PROVEN_OK : PROVEN_ERR_IO;
        }
        if (err == PROVEN_OK) {
            rp_sock_t *s = rp_mem_alloc(alloc, 1, sizeof *s);
            if (s == NULL) {
                closesocket(fd);
                err = PROVEN_ERR_NOMEM;
                break;
            }
            s->alloc = alloc;
            s->fd = fd;
            *out = s;
        } else {
            closesocket(fd);
        }
    }
    FreeAddrInfoW(list);
    return err;
}

proven_err_t rp_pal_tcp_send(rp_sock_t *s, const uint8_t *data, size_t len, int timeout_ms) {
    int64_t deadline = rp_pal_now_ms() + timeout_ms;
    while (len) {
        proven_err_t err = wait_sock(s->fd, POLLWRNORM, (int)(deadline - rp_pal_now_ms()));
        if (err != PROVEN_OK) return err;
        int n = send(s->fd, (const char *)data, len > 0x10000000u ? 0x10000000 : (int)len, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
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
        proven_err_t err = wait_sock(s->fd, POLLRDNORM, timeout_ms);
        if (err != PROVEN_OK) return err;
        int n = recv(s->fd, (char *)buf, cap > 0x10000000u ? 0x10000000 : (int)cap, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return PROVEN_ERR_IO;
        }
        *got = (size_t)n;
        return PROVEN_OK;
    }
}

void rp_pal_tcp_close(rp_sock_t *s) {
    if (s == NULL) return;
    closesocket(s->fd);
    rp_mem_free(s->alloc, s);
}

proven_err_t rp_pal_system_roots(proven_allocator_t alloc, void (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx) {
    (void)alloc;
    // The ROOT system store: the local machine's roots and the user's. Windows adds some roots
    // only when something first needs them (automatic root update), so a root can be missing here.
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == NULL) return PROVEN_ERR_NOT_FOUND;
    size_t count = 0;
    for (PCCERT_CONTEXT c = CertEnumCertificatesInStore(store, NULL); c; c = CertEnumCertificatesInStore(store, c)) {
        sink(ctx, c->pbCertEncoded, c->cbCertEncoded);
        ++count;
    }
    CertCloseStore(store, 0);
    return count ? PROVEN_OK : PROVEN_ERR_NOT_FOUND;
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
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (size_t)si.dwNumberOfProcessors : 1;
}

static DWORD WINAPI rp_work_thread(LPVOID arg) {
    rp_work_run(arg);
    return 0;
}

void rp_pal_parallel_for(size_t jobs, size_t count, void (*fn)(void *ctx, size_t i), void *ctx) {
    rp_work_t w = { .fn = fn, .ctx = ctx, .count = count };
    atomic_init(&w.next, 0);
    if (jobs > count) jobs = count;
    if (jobs > 64) jobs = 64;
    HANDLE t[64];
    size_t started = 0;
    for (size_t k = 1; k < jobs; ++k) {
        HANDLE h = CreateThread(NULL, 0, rp_work_thread, &w, 0, NULL);
        if (h) t[started++] = h;
    }
    rp_work_run(&w);
    for (size_t k = 0; k < started; ++k) {
        WaitForSingleObject(t[k], INFINITE);
        CloseHandle(t[k]);
    }
}

// ---- mapped files (RFC-0013 E1) ------------------------------------------------------------------

struct rp_outmap {
    HANDLE          file, mapping;
    void           *view;
    proven_u16str_t tmp, path;
};

static void outmap_close(rp_outmap_t *m) {
    if (m->view) UnmapViewOfFile(m->view);
    if (m->mapping) CloseHandle(m->mapping);
    if (m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
    m->view = NULL;
    m->mapping = NULL;
    m->file = INVALID_HANDLE_VALUE;
}

static void outmap_free(proven_allocator_t alloc, rp_outmap_t *m) {
    proven_u16str_destroy(alloc, &m->tmp);
    proven_u16str_destroy(alloc, &m->path);
    rp_mem_free(alloc, m);
}

proven_err_t rp_pal_outmap_create(proven_allocator_t alloc, const char *path_utf8, size_t len, uint8_t **data, rp_outmap_t **om) {
    if (path_utf8 == NULL || data == NULL || om == NULL || len == 0 || len > (size_t)INT64_MAX) return PROVEN_ERR_INVALID_ARG;
    *data = NULL;
    *om = NULL;
    size_t n = strlen(path_utf8);
    char *tmp = rp_mem_alloc(alloc, n + 16, 1);
    rp_outmap_t *m = rp_mem_alloc(alloc, 1, sizeof *m);
    if (tmp == NULL || m == NULL) {
        rp_mem_free(alloc, tmp);
        rp_mem_free(alloc, m);
        return PROVEN_ERR_NOMEM;
    }
    *m = (rp_outmap_t){ .file = INVALID_HANDLE_VALUE };
    memcpy(tmp, path_utf8, n);
    memcpy(tmp + n, ".rp-map", 8);
    proven_err_t err = file_path(alloc, path_utf8, &m->path);
    if (err == PROVEN_OK) err = file_path(alloc, tmp, &m->tmp);
    rp_mem_free(alloc, tmp);
    if (err != PROVEN_OK) {
        outmap_free(alloc, m);
        return err;
    }
    m->file = CreateFileW((const wchar_t *)proven_u16str_as_ptr(&m->tmp), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->file == INVALID_HANDLE_VALUE) {
        outmap_free(alloc, m);
        return PROVEN_ERR_IO;
    }
    // The length is set (and its clusters allocated) before mapping: a full disk is an error here.
    LARGE_INTEGER size = { .QuadPart = (LONGLONG)len };
    if (!SetFilePointerEx(m->file, size, NULL, FILE_BEGIN) || !SetEndOfFile(m->file) ||
        (m->mapping = CreateFileMappingW(m->file, NULL, PAGE_READWRITE, 0, 0, NULL)) == NULL ||
        (m->view = MapViewOfFile(m->mapping, FILE_MAP_WRITE, 0, 0, 0)) == NULL) {
        rp_pal_outmap_discard(alloc, m);
        return PROVEN_ERR_IO;
    }
    *data = m->view;
    *om = m;
    return PROVEN_OK;
}

proven_err_t rp_pal_outmap_commit(proven_allocator_t alloc, rp_outmap_t *om) {
    if (om == NULL) return PROVEN_ERR_INVALID_ARG;
    proven_err_t err = PROVEN_OK;
    if (!FlushViewOfFile(om->view, 0)) err = PROVEN_ERR_IO;
    UnmapViewOfFile(om->view);
    om->view = NULL;
    CloseHandle(om->mapping);
    om->mapping = NULL;
    if (!FlushFileBuffers(om->file)) err = PROVEN_ERR_IO;
    outmap_close(om);
    const wchar_t *wt = (const wchar_t *)proven_u16str_as_ptr(&om->tmp);
    if (err == PROVEN_OK &&
        !MoveFileExW(wt, (const wchar_t *)proven_u16str_as_ptr(&om->path), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = PROVEN_ERR_IO;
    }
    if (err != PROVEN_OK) DeleteFileW(wt);
    outmap_free(alloc, om);
    return err;
}

void rp_pal_outmap_discard(proven_allocator_t alloc, rp_outmap_t *om) {
    if (om == NULL) return;
    outmap_close(om);
    DeleteFileW((const wchar_t *)proven_u16str_as_ptr(&om->tmp));
    outmap_free(alloc, om);
}
