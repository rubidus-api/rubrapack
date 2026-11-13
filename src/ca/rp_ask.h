// rp_ask.h - a small window with a text and two choices beside Cancel: the question an already
// installed product is met with (the helper DLL for a package without dialogs, setup.exe), and the
// preflight's questions and messages (the helper DLL). Made from a dialog template in memory, so
// neither part carries resources. The window grows to its text (a list of programs, the folders
// left behind): Windows Installer's own message box cut such a text with "..." (owner 2026-11-13);
// past 60% of the screen the text scrolls.

#ifndef RP_ASK_H
#define RP_ASK_H

#include <windows.h>

#include <stdint.h>
#include <string.h>

typedef struct {
    const wchar_t *title, *text, *first, *second, *cancel;     // first NULL: one button (cancel, as OK)
    bool           rtl;     // the texts read right to left: the window is mirrored
    int            def;     // the default button: 0 first (the default), 2 second, 3 cancel
} rp_ask_t;

// Whether a language reads right to left (LOCALE_IREADINGLAYOUT 1), and the message box flags for it.
static inline bool rp_lcid_rtl(LCID lcid) {
    DWORD v = 0;
    return lcid && GetLocaleInfoW(lcid, LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER, (LPWSTR)&v, sizeof v / sizeof(wchar_t)) && v == 1;
}
static inline UINT rp_mb_rtl(bool rtl) { return rtl ? MB_RTLREADING | MB_RIGHT : 0; }

enum { RP_ASK_FAILED = -1, RP_ASK_CANCEL = 0, RP_ASK_FIRST = 1, RP_ASK_SECOND = 2, RP_ASK_ID_TEXT = 100, RP_ASK_ID_FIRST = 101, RP_ASK_ID_SECOND = 102 };

// `text` with every line end as CR LF (an edit control shows a lone LF as nothing), on the heap.
static wchar_t *rp_ask_crlf(const wchar_t *text) {
    size_t n = 0, lf = 0;
    for (; text[n]; ++n) lf += text[n] == L'\n';
    wchar_t *out = HeapAlloc(GetProcessHeap(), 0, (n + lf + 1) * sizeof *out);
    if (out == NULL) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) out[o++] = L'\r';
        out[o++] = text[i];
    }
    out[o] = 0;
    return out;
}

// Moves control `id` of `w` down by `dy`.
static void rp_ask_down(HWND w, int id, int dy) {
    HWND c = GetDlgItem(w, id);
    RECT r;
    if (c == NULL || !GetWindowRect(c, &r)) return;
    // Two points into a mirrored window are taken as a RECT and its sides swapped back: r.left is
    // the control's x there too.
    MapWindowPoints(NULL, w, (POINT *)&r, 2);
    SetWindowPos(c, NULL, r.left, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static INT_PTR CALLBACK rp_ask_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_INITDIALOG) {
        const rp_ask_t *a = (const rp_ask_t *)lp;
        SetWindowTextW(w, a->title);
        wchar_t *text = rp_ask_crlf(a->text ? a->text : L"");
        const wchar_t *shown = text ? text : (a->text ? a->text : L"");
        SetDlgItemTextW(w, RP_ASK_ID_TEXT, shown);
        if (a->first) {
            SetDlgItemTextW(w, RP_ASK_ID_FIRST, a->first);
            SetDlgItemTextW(w, RP_ASK_ID_SECOND, a->second);
        } else {
            ShowWindow(GetDlgItem(w, RP_ASK_ID_FIRST), SW_HIDE);
            ShowWindow(GetDlgItem(w, RP_ASK_ID_SECOND), SW_HIDE);
        }
        SetDlgItemTextW(w, IDCANCEL, a->cancel);
        // The text's height as it wraps in its box; the box, the buttons and the window follow.
        HWND t = GetDlgItem(w, RP_ASK_ID_TEXT);
        RECT tr, wr;
        if (t && GetWindowRect(t, &tr) && GetWindowRect(w, &wr)) {
            int width = tr.right - tr.left, have = tr.bottom - tr.top;
            HDC dc = GetDC(t);
            HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(t, WM_GETFONT, 0, 0));
            RECT need = { 0, 0, width - GetSystemMetrics(SM_CXVSCROLL), 0 };
            DrawTextW(dc, shown, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            TEXTMETRICW tm;
            int line = GetTextMetricsW(dc, &tm) ? tm.tmHeight : 16;
            SelectObject(dc, old);
            ReleaseDC(t, dc);
            int want = need.bottom + line / 2;
            MONITORINFO mi = { .cbSize = sizeof mi };
            int cap = GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) ? (mi.rcWork.bottom - mi.rcWork.top) * 6 / 10 : 600;
            bool scroll = want > cap;
            if (scroll) want = cap;
            if (want > have) {
                int dy = want - have;
                SetWindowPos(t, NULL, 0, 0, width, want, SWP_NOMOVE | SWP_NOZORDER);
                rp_ask_down(w, RP_ASK_ID_FIRST, dy);
                rp_ask_down(w, RP_ASK_ID_SECOND, dy);
                rp_ask_down(w, IDCANCEL, dy);
                int wh = wr.bottom - wr.top + dy, ww = wr.right - wr.left;
                int top = mi.rcWork.bottom > mi.rcWork.top ? mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2 : wr.top - dy / 2;
                SetWindowPos(w, NULL, wr.left, top, ww, wh, SWP_NOZORDER);
            }
            ShowScrollBar(t, SB_VERT, scroll);      // after the resize, which has the box redo its scroll bar
        }
        if (text) HeapFree(GetProcessHeap(), 0, text);
        int focus = a->first == NULL || a->def == 3 ? IDCANCEL : a->def == 2 ? RP_ASK_ID_SECOND : RP_ASK_ID_FIRST;
        SendMessageW(w, DM_SETDEFID, (WPARAM)focus, 0);
        // The window comes from a process in the background (the installer's action server), which
        // Windows does not let take the foreground: it would stay inactive and keys (Enter, the
        // buttons' letters) would not reach it. Joined for a moment to the input of the window in
        // front (the installer's own), it may.
        HWND fg = GetForegroundWindow();
        DWORD them = fg ? GetWindowThreadProcessId(fg, NULL) : 0, me = GetCurrentThreadId();
        bool joined = them && them != me && AttachThreadInput(me, them, TRUE);
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(w);
        SetActiveWindow(w);
        SetFocus(GetDlgItem(w, focus));
        if (joined) AttachThreadInput(me, them, FALSE);
        if (GetForegroundWindow() != w) {
            FLASHWINFO fi = { .cbSize = sizeof fi, .hwnd = w, .dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG };
            FlashWindowEx(&fi);
        }
        return FALSE;               // the focus is set here
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

// RP_ASK_FIRST, RP_ASK_SECOND, RP_ASK_CANCEL, or RP_ASK_FAILED when the window cannot be made. The
// buttons are 76 units wide: every pack's Repair / Remove / Cancel fits (T158, RFC-0031 L13).
static int rp_ask_raw(HWND owner, const rp_ask_t *a) {
    static const wchar_t face[] = L"Segoe UI";
    DWORD buf[96];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE t = { .style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                      .dwExtendedStyle = WS_EX_TOPMOST | (a->rtl ? WS_EX_LAYOUTRTL : 0), .cdit = 4, .cx = 262, .cy = 66 };
    memcpy(buf, &t, sizeof t);
    WORD *p = (WORD *)((uint8_t *)buf + sizeof t);
    *p++ = 0;               // no menu
    *p++ = 0;               // the dialog class
    *p++ = 0;               // no title yet
    *p++ = 9;
    memcpy(p, face, sizeof face);
    p += sizeof face / sizeof face[0];
    DWORD rd = a->rtl ? WS_EX_RTLREADING : 0;       // the mirrored window puts the text at the right
    // The text: a read-only edit box without a border (it can scroll a long list), 0x0081.
    p = rp_ask_item(p, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, rd, 10, 8, 242, 28, RP_ASK_ID_TEXT, 0x0081);
    p = rp_ask_item(p, BS_PUSHBUTTON | WS_TABSTOP, rd, 10, 44, 76, 14, RP_ASK_ID_FIRST, 0x0080);
    p = rp_ask_item(p, BS_PUSHBUTTON | WS_TABSTOP, rd, 93, 44, 76, 14, RP_ASK_ID_SECOND, 0x0080);
    p = rp_ask_item(p, BS_PUSHBUTTON | WS_TABSTOP, rd, 176, 44, 76, 14, IDCANCEL, 0x0080);
    (void)p;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)buf, owner, rp_ask_proc, (LPARAM)a);
    return r == RP_ASK_FIRST || r == RP_ASK_SECOND ? (int)r : r == -1 ? RP_ASK_FAILED : RP_ASK_CANCEL;
}

// rp_ask_raw, with a window that cannot be made taken as Cancel.
[[maybe_unused]] static int rp_ask(HWND owner, const rp_ask_t *a) {
    int r = rp_ask_raw(owner, a);
    return r == RP_ASK_FAILED ? RP_ASK_CANCEL : r;
}

#endif
