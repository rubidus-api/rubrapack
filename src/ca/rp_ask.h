// rp_ask.h - a small window with a text and two choices beside Cancel: the question an already
// installed product is met with (the helper DLL for a package without dialogs, setup.exe).
// Made from a dialog template in memory, so neither part carries resources.

#ifndef RP_ASK_H
#define RP_ASK_H

#include <windows.h>

#include <stdint.h>
#include <string.h>

typedef struct {
    const wchar_t *title, *text, *first, *second, *cancel;
    bool           rtl;     // the texts read right to left: the window is mirrored
} rp_ask_t;

// Whether a language reads right to left (LOCALE_IREADINGLAYOUT 1), and the message box flags for it.
static inline bool rp_lcid_rtl(LCID lcid) {
    DWORD v = 0;
    return lcid && GetLocaleInfoW(lcid, LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER, (LPWSTR)&v, sizeof v / sizeof(wchar_t)) && v == 1;
}
static inline UINT rp_mb_rtl(bool rtl) { return rtl ? MB_RTLREADING | MB_RIGHT : 0; }

enum { RP_ASK_CANCEL = 0, RP_ASK_FIRST = 1, RP_ASK_SECOND = 2, RP_ASK_ID_TEXT = 100, RP_ASK_ID_FIRST = 101, RP_ASK_ID_SECOND = 102 };

static INT_PTR CALLBACK rp_ask_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_INITDIALOG) {
        const rp_ask_t *a = (const rp_ask_t *)lp;
        SetWindowTextW(w, a->title);
        SetDlgItemTextW(w, RP_ASK_ID_TEXT, a->text);
        SetDlgItemTextW(w, RP_ASK_ID_FIRST, a->first);
        SetDlgItemTextW(w, RP_ASK_ID_SECOND, a->second);
        SetDlgItemTextW(w, IDCANCEL, a->cancel);
        SetForegroundWindow(w);
        return TRUE;
    }
    if (m == WM_COMMAND) {
        WORD id = LOWORD(wp);
        if (id == RP_ASK_ID_FIRST || id == RP_ASK_ID_SECOND || id == IDCANCEL) {
            EndDialog(w, id == RP_ASK_ID_FIRST ? RP_ASK_FIRST : id == RP_ASK_ID_SECOND ? RP_ASK_SECOND : RP_ASK_CANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

// One control: the class by its atom (0x0080 button, 0x0082 static), no text (set when the window opens).
static WORD *rp_ask_item(WORD *p, DWORD style, DWORD ex, short x, short y, short cx, short cy, WORD id, WORD cls) {
    p = (WORD *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    DLGITEMTEMPLATE it = { .style = style | WS_CHILD | WS_VISIBLE, .dwExtendedStyle = ex, .x = x, .y = y, .cx = cx, .cy = cy, .id = id };
    memcpy(p, &it, sizeof it);
    p = (WORD *)((uint8_t *)p + sizeof it);
    *p++ = 0xFFFF;
    *p++ = cls;
    *p++ = 0;
    *p++ = 0;
    return p;
}

// RP_ASK_FIRST, RP_ASK_SECOND or RP_ASK_CANCEL (also when the window cannot be made).
static int rp_ask(HWND owner, const rp_ask_t *a) {
    static const wchar_t face[] = L"Segoe UI";
    DWORD buf[96];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE t = { .style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                      .dwExtendedStyle = WS_EX_TOPMOST | (a->rtl ? WS_EX_LAYOUTRTL : 0), .cdit = 4, .cx = 232, .cy = 66 };
    memcpy(buf, &t, sizeof t);
    WORD *p = (WORD *)((uint8_t *)buf + sizeof t);
    *p++ = 0;               // no menu
    *p++ = 0;               // the dialog class
    *p++ = 0;               // no title yet
    *p++ = 9;
    memcpy(p, face, sizeof face);
    p += sizeof face / sizeof face[0];
    DWORD rd = a->rtl ? WS_EX_RTLREADING : 0;       // the mirrored window puts the text at the right
    p = rp_ask_item(p, SS_LEFT | SS_NOPREFIX, rd, 10, 8, 212, 28, RP_ASK_ID_TEXT, 0x0082);
    p = rp_ask_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, rd, 10, 44, 66, 14, RP_ASK_ID_FIRST, 0x0080);
    p = rp_ask_item(p, BS_PUSHBUTTON | WS_TABSTOP, rd, 83, 44, 66, 14, RP_ASK_ID_SECOND, 0x0080);
    p = rp_ask_item(p, BS_PUSHBUTTON | WS_TABSTOP, rd, 156, 44, 66, 14, IDCANCEL, 0x0080);
    (void)p;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)buf, owner, rp_ask_proc, (LPARAM)a);
    return r == RP_ASK_FIRST || r == RP_ASK_SECOND ? (int)r : RP_ASK_CANCEL;
}

#endif
