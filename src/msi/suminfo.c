// src/msi/suminfo.c - summary information reader and IDT export (include/rubrapack/suminfo.h).

#include "rubrapack/buf.h"
#include "rubrapack/suminfo.h"

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
            rp_buf_free(&b);
            return PROVEN_ERR_UNSUPPORTED;      // no oracle yet for the date text
        } else {
            rp_buf_long(&b, p->i);
        }
        rp_buf_puts(&b, "\r\n");
    }
    return rp_buf_take(&b, out, len);
}
