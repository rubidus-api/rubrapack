// src/msi/suminfo.c - summary information reader and IDT export (include/rubrapack/suminfo.h).

#include "rubrapack/buf.h"
#include "rubrapack/suminfo.h"

#include <stdio.h>
#include <string.h>

// FMTID_SummaryInformation {F29F85E0-4FF9-1068-AB91-08002B27B3D9} as stored.
static const uint8_t fmtid_summary[16] = { 0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10,
                                           0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

proven_err_t rp_suminfo_parse(const uint8_t *data, size_t len, rp_suminfo_t *out) {
    if (data == NULL || out == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (len < 48 || rd16(data) != 0xFFFE) return PROVEN_ERR_INVALID_FORMAT;
    if (rd32(data + 24) < 1 || memcmp(data + 28, fmtid_summary, 16) != 0) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t sec = rd32(data + 44);
    if (sec > len || len - sec < 8) return PROVEN_ERR_INVALID_FORMAT;
    uint32_t sec_size = rd32(data + sec), count = rd32(data + sec + 4);
    if (sec_size > len - sec || count > RP_SUMINFO_MAX || (uint64_t)count * 8 > sec_size - 8) {
        return PROVEN_ERR_INVALID_FORMAT;
    }
    const uint8_t *s = data + sec;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t pid = rd32(s + 8 + 8 * k), off = rd32(s + 12 + 8 * k);
        if (off > sec_size || sec_size - off < 8) return PROVEN_ERR_INVALID_FORMAT;
        const uint8_t *v = s + off;
        size_t room = sec_size - off - 4;
        rp_suminfo_prop_t *p = &out->props[out->count++];
        p->pid = pid;
        p->type = rd16(v);
        switch (p->type) {
        case RP_VT_I2:
            p->i = (int16_t)rd16(v + 4);
            break;
        case RP_VT_I4:
            p->i = (int32_t)rd32(v + 4);
            break;
        case RP_VT_FILETIME:
            if (room < 8) return PROVEN_ERR_INVALID_FORMAT;
            p->filetime = (uint64_t)rd32(v + 4) | ((uint64_t)rd32(v + 8) << 32);
            break;
        case RP_VT_LPSTR: {
            uint32_t n = rd32(v + 4);
            if (n > room - 4) return PROVEN_ERR_INVALID_FORMAT;
            p->str = v + 8;
            p->str_len = n;
            while (p->str_len > 0 && p->str[p->str_len - 1] == 0) --p->str_len;   // drop the NUL
            break;
        }
        default:
            return PROVEN_ERR_UNSUPPORTED;
        }
    }
    return PROVEN_OK;
}

void rp_filetime_text(uint64_t ft, char out[20]) {
    int64_t secs = (int64_t)(ft / 10000000u) - INT64_C(11644473600);   // to Unix time
    int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    int64_t rem = secs - days * 86400;
    // Civil date from days since 1970-01-01 (proleptic Gregorian).
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    if (y < 0 || y > 9999) y = 0;       // out of range: printed as year 0000
    int64_t parts[6] = { y, m, d, rem / 3600, rem % 3600 / 60, rem % 60 };
    static const char seps[6] = { '/', '/', ' ', ':', ':', 0 };
    size_t o = 0;
    for (int k = 0; k < 6; ++k) {
        int width = k == 0 ? 4 : 2;
        for (int w = width - 1; w >= 0; --w) {
            int64_t v = parts[k];
            for (int q = 0; q < w; ++q) v /= 10;
            out[o++] = (char)('0' + v % 10);
        }
        out[o++] = seps[k];
    }
}

proven_err_t rp_suminfo_export_idt(const rp_suminfo_t *si, proven_allocator_t alloc, uint8_t **out, size_t *len) {
    if (si == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    // Rows go out in property id order.
    size_t order[RP_SUMINFO_MAX];
    for (size_t i = 0; i < si->count; ++i) order[i] = i;
    for (size_t i = 1; i < si->count; ++i) {
        for (size_t j = i; j > 0 && si->props[order[j - 1]].pid > si->props[order[j]].pid; --j) {
            size_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    rp_buf_t b = rp_buf_new(alloc, 1u << 20);
    rp_buf_puts(&b, "PropertyId\tValue\r\ni2\tl255\r\n_SummaryInformation\tPropertyId\r\n");
    for (size_t k = 0; k < si->count; ++k) {
        const rp_suminfo_prop_t *p = &si->props[order[k]];
        rp_buf_long(&b, p->pid);
        rp_buf_byte(&b, '\t');
        if (p->type == RP_VT_LPSTR) {
            rp_buf_put(&b, p->str, p->str_len);
        } else if (p->type == RP_VT_FILETIME) {
            char t[20];
            rp_filetime_text(p->filetime, t);
            rp_buf_puts(&b, t);
        } else {
            rp_buf_long(&b, p->i);
        }
        rp_buf_puts(&b, "\r\n");
    }
    return rp_buf_take(&b, out, len);
}

proven_err_t rp_suminfo_write(const rp_suminfo_t *si, bool utf16, proven_allocator_t alloc, uint8_t **out, size_t *len) {
    if (si == NULL || out == NULL || len == NULL || si->count > RP_SUMINFO_MAX) return PROVEN_ERR_INVALID_ARG;
    // Value sizes first, to lay out the offset table.
    size_t sizes[RP_SUMINFO_MAX];
    size_t term = utf16 ? 2 : 1;
    for (size_t k = 0; k < si->count; ++k) {
        const rp_suminfo_prop_t *p = &si->props[k];
        switch (p->type) {
        case RP_VT_I2:
        case RP_VT_I4: sizes[k] = 8; break;
        case RP_VT_FILETIME: sizes[k] = 12; break;
        case RP_VT_LPSTR:
            if (p->str_len > 0xFFFF || (p->str_len && p->str == NULL)) return PROVEN_ERR_INVALID_ARG;
            sizes[k] = 8 + ((p->str_len + term + 3) & ~(size_t)3);
            break;
        default: return PROVEN_ERR_UNSUPPORTED;
        }
    }
    size_t section = 8 + 8 * si->count;
    for (size_t k = 0; k < si->count; ++k) section += sizes[k];

    rp_buf_t b = rp_buf_new(alloc, 1u << 20);
    rp_buf_u16le(&b, 0xFFFE);
    rp_buf_u16le(&b, 0);                    // version 0, as msi.dll writes
    rp_buf_u32le(&b, 0x00020206);           // OS field seen from msi.dll
    rp_buf_zero(&b, 16);                    // CLSID
    rp_buf_u32le(&b, 1);                    // one section
    rp_buf_put(&b, fmtid_summary, 16);
    rp_buf_u32le(&b, 48);
    rp_buf_u32le(&b, (uint32_t)section);
    rp_buf_u32le(&b, (uint32_t)si->count);
    size_t off = 8 + 8 * si->count;
    for (size_t k = 0; k < si->count; ++k) {
        rp_buf_u32le(&b, si->props[k].pid);
        rp_buf_u32le(&b, (uint32_t)off);
        off += sizes[k];
    }
    for (size_t k = 0; k < si->count; ++k) {
        const rp_suminfo_prop_t *p = &si->props[k];
        rp_buf_u32le(&b, p->type);
        switch (p->type) {
        case RP_VT_I2:
            rp_buf_u16le(&b, (uint16_t)p->i);
            rp_buf_u16le(&b, 0);
            break;
        case RP_VT_I4: rp_buf_u32le(&b, (uint32_t)p->i); break;
        case RP_VT_FILETIME: rp_buf_u64le(&b, p->filetime); break;
        default: {
            size_t n = p->str_len + term;
            rp_buf_u32le(&b, (uint32_t)n);
            rp_buf_put(&b, p->str, p->str_len);
            rp_buf_zero(&b, sizes[k] - 8 - p->str_len);
        }
        }
    }
    return rp_buf_take(&b, out, len);
}
