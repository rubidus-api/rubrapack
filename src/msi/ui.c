// src/msi/ui.c - the built-in dialog sets (include/rubrapack/ui.h, RFC-0005).
//
// Every dialog is 370 x 270 dialog units: a white banner (44 high) with a bold title and a line of
// description, the body, a line at 234 and the buttons Back / Next / Cancel at y 243. Nothing is
// drawn from bitmaps except the banner fill (a 1 x 1 white BMP, stretched) unless the source gives
// its own banner. The wording below is rubrapack's own, in Korean and English.

#include "rubrapack/ui.h"
#include "rubrapack/mem.h"

#include <stdio.h>
#include <string.h>

// ---- texts ------------------------------------------------------------------------------------

typedef struct {
    const char *id, *ko, *en;
} text_t;

static const text_t texts[] = {
    { "Back", "< 뒤로(&B)", "< &Back" },
    { "Next", "다음(&N) >", "&Next >" },
    { "Cancel", "취소", "Cancel" },
    { "Install", "설치(&I)", "&Install" },
    { "Finish", "마침(&F)", "&Finish" },
    { "OK", "확인", "OK" },
    { "Yes", "예(&Y)", "&Yes" },
    { "No", "아니요(&N)", "&No" },
    { "Retry", "다시 시도(&R)", "&Retry" },
    { "Ignore", "무시(&I)", "&Ignore" },
    { "Abort", "중단(&A)", "&Abort" },
    { "Exit", "끝내기(&X)", "E&xit" },
    { "Browse", "찾아보기(&R)...", "B&rowse..." },
    { "WelcomeTitle", "[ProductName] 설치", "Welcome to [ProductName] Setup" },
    { "WelcomeText", "이 프로그램은 [ProductName]을(를) 이 컴퓨터에 설치합니다. 계속하려면 '다음'을 누르십시오.",
      "This will install [ProductName] on your computer. Click Next to continue." },
    { "LicenseTitle", "사용권 계약", "License agreement" },
    { "LicenseText", "다음 사용권 계약을 읽어 주십시오.", "Please read the following license agreement." },
    { "LicenseAccept", "사용권 계약에 동의합니다(&A)", "I &accept the terms of the license agreement" },
    { "DirTitle", "설치 폴더", "Installation folder" },
    { "DirText", "[ProductName]을(를) 설치할 폴더를 고르십시오.", "Choose the folder to install [ProductName] in." },
    { "DirLabel", "설치 폴더(&F):", "Install &to:" },
    { "BrowseTitle", "폴더 바꾸기", "Change the folder" },
    { "BrowseText", "설치할 폴더를 찾아 고르십시오.", "Browse to the folder to install in." },
    { "BrowseLookIn", "찾는 위치(&L):", "&Look in:" },
    { "BrowseFolder", "폴더 이름(&N):", "&Folder name:" },
    { "BrowseUp", "위로", "Up" },
    { "BrowseNew", "새 폴더", "New folder" },
    { "CustomizeTitle", "기능 고르기", "Choose features" },
    { "CustomizeText", "설치할 기능을 고르십시오.", "Choose the features to install." },
    { "Reset", "처음대로(&S)", "Re&set" },
    { "DiskCost", "디스크 공간(&D)", "&Disk usage" },
    { "DiskCostTitle", "디스크 공간", "Disk space" },
    { "DiskCostText", "고른 기능을 설치할 공간과 남은 공간입니다.", "Space needed for the chosen features, and space available." },
    { "ReadyTitle", "설치 준비 완료", "Ready to install" },
    { "ReadyText", "'설치'를 누르면 [ProductName]을(를) 설치합니다.", "Click Install to install [ProductName]." },
    { "ProgressTitle", "[ProductName] 설치 중", "Installing [ProductName]" },
    { "ProgressText", "잠시 기다려 주십시오.", "Please wait." },
    { "ProgressStatus", "상태:", "Status:" },
    { "ExitTitle", "설치 완료", "Setup complete" },
    { "ExitText", "[ProductName]을(를) 설치했습니다.", "[ProductName] has been installed." },
    { "UserExitTitle", "설치 취소", "Setup cancelled" },
    { "UserExitText", "설치를 취소했습니다. 컴퓨터는 바뀌지 않았습니다.", "Setup was cancelled. Nothing on your computer was changed." },
    { "FatalTitle", "설치 실패", "Setup failed" },
    { "FatalText", "설치 중 오류가 나서 설치를 되돌렸습니다. 컴퓨터는 바뀌지 않았습니다.",
      "An error ended the installation, which was rolled back. Nothing on your computer was changed." },
    { "CancelText", "[ProductName] 설치를 취소하시겠습니까?", "Cancel the installation of [ProductName]?" },
    { "FilesInUseTitle", "사용 중인 파일", "Files in use" },
    { "FilesInUseText", "다음 프로그램이 바꿔야 할 파일을 쓰고 있습니다. 닫고 '다시 시도'를 누르거나, '무시'를 눌러 다음 재시작 때 바꾸게 하십시오.",
      "These programs use files that need to be changed. Close them and click Retry, or click Ignore to have them replaced at the next restart." },
    { "OutOfDiskTitle", "디스크 공간 부족", "Not enough disk space" },
    { "OutOfDiskText", "고른 설치에 필요한 공간이 모자랍니다. 파일을 지우거나 다른 위치를 고르십시오.",
      "There is not enough space for this installation. Free some space or choose another location." },
    { "MaintTitle", "[ProductName] 관리", "Maintain [ProductName]" },
    { "MaintText", "할 일을 고르십시오.", "Choose what to do." },
    { "Repair", "복구(&P)", "Re&pair" },
    { "RepairText", "빠지거나 망가진 파일과 설정을 되살립니다.", "Restore missing or damaged files and settings." },
    { "Remove", "제거(&R)", "&Remove" },
    { "RemoveText", "[ProductName]을(를) 이 컴퓨터에서 지웁니다.", "Remove [ProductName] from this computer." },
};

bool rp_ui_text_known(const char *id) {
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        if (strcmp(texts[i].id, id) == 0) return true;
    }
    return false;
}

// UIText rows the engine asks for (sizes, selection tree menus, volume list columns).
static const text_t uitexts[] = {
    { "AbsentPath", "", "" },
    { "bytes", "바이트", "bytes" },
    { "GB", "GB", "GB" },
    { "KB", "KB", "KB" },
    { "MB", "MB", "MB" },
    { "MenuAbsent", "이 기능을 설치하지 않음", "Entire feature will be unavailable" },
    { "MenuAllLocal", "이 기능과 하위 기능 모두 이 컴퓨터에 설치", "Entire feature will be installed on local hard drive" },
    { "MenuLocal", "이 기능을 이 컴퓨터에 설치", "Will be installed on local hard drive" },
    { "MenuAdvertise", "필요할 때 설치", "Will be installed when required" },
    { "NewFolder", "새폴더|새 폴더", "Folder|New Folder" },
    { "SelAbsentAbsent", "설치되지 않은 채로 둡니다.", "This feature will remain uninstalled." },
    { "SelAbsentLocal", "이 컴퓨터에 설치합니다.", "This feature will be installed on the local hard drive." },
    { "SelLocalAbsent", "지웁니다.", "This feature will be removed." },
    { "SelLocalLocal", "이 컴퓨터에 설치된 채로 둡니다.", "This feature will remain on your local hard drive." },
    { "SelChildCostNeg", "[1] 줄어듭니다.", "This feature frees up [1] on your hard drive." },
    { "SelChildCostPos", "[1] 필요합니다.", "This feature requires [1] on your hard drive." },
    { "SelCostPending", "계산 중...", "Compiling cost for this feature..." },
    { "SelParentCostNegNeg", "이 기능은 [1] 줄어듭니다. 하위 기능 [2]/[3]개가 [4] 줄어듭니다.",
      "This feature frees up [1]. It has [2] of [3] subfeatures selected, which free up [4]." },
    { "SelParentCostNegPos", "이 기능은 [1] 줄어듭니다. 하위 기능 [2]/[3]개가 [4] 필요합니다.",
      "This feature frees up [1]. It has [2] of [3] subfeatures selected, which require [4]." },
    { "SelParentCostPosNeg", "이 기능은 [1] 필요합니다. 하위 기능 [2]/[3]개가 [4] 줄어듭니다.",
      "This feature requires [1]. It has [2] of [3] subfeatures selected, which free up [4]." },
    { "SelParentCostPosPos", "이 기능은 [1] 필요합니다. 하위 기능 [2]/[3]개가 [4] 필요합니다.",
      "This feature requires [1]. It has [2] of [3] subfeatures selected, which require [4]." },
    { "TimeRemaining", "남은 시간: {[1]분 }{[2]초}", "Time remaining: {[1] minutes }{[2] seconds}" },
    { "VolumeCostAvailable", "남은 공간", "Available" },
    { "VolumeCostDifference", "차이", "Difference" },
    { "VolumeCostRequired", "필요한 공간", "Required" },
    { "VolumeCostSize", "디스크 크기", "Disk size" },
    { "VolumeCostVolume", "볼륨", "Volume" },
};

// ---- rows --------------------------------------------------------------------------------------

typedef struct {
    rp_msi_wcolumn_t const *cols;
    size_t                  ncols;
    const char             *name;
    rp_msi_cell_t          *cells;
    size_t                  filled, cap;
} rows_t;

// A 32 x 32, 32-bit ICO: 6-byte header, one 16-byte entry, a 40-byte info header, the pixels, and
// the (unused, all zero) AND mask - the alpha channel does the masking.
enum { ICON_PIXELS = 32 * 32 * 4, ICON_SIZE = 6 + 16 + 40 + ICON_PIXELS + 32 * 4 };

typedef struct {
    proven_allocator_t alloc;
    const rp_ir_t     *ir;
    bool               ko;
    bool               nomem;
    char             **strings;
    size_t             nstr, capstr;
    rows_t             dialog, control, event, condition, mapping, style, uitext, binary, radio, combo;
    rp_ui_prop_t       props[8];
    size_t             nprops;
    rp_ui_seq_t        seqs[16];
    size_t             nseqs;
    uint8_t            white[58];
    uint8_t            icon[ICON_SIZE];
} ctx_t;

#define KEY_S(n) ((uint16_t)(0x2D00 | (n)))
#define S(n)     ((uint16_t)(0x0D00 | (n)))
#define S_N(n)   ((uint16_t)(0x1D00 | (n)))
#define L(n)     ((uint16_t)(0x0F00 | (n)))
#define L_N(n)   ((uint16_t)(0x1F00 | (n)))
#define I2       ((uint16_t)0x0502)
#define I2_N     ((uint16_t)0x1502)
#define I4_N     ((uint16_t)0x1104)
#define KEY_I2   ((uint16_t)0x2502)
#define KEY_S_N(n) ((uint16_t)(0x3D00 | (n)))

static const rp_msi_wcolumn_t dialog_cols[] = { { "Dialog", KEY_S(72) }, { "HCentering", I2 }, { "VCentering", I2 },
    { "Width", I2 }, { "Height", I2 }, { "Attributes", I4_N }, { "Title", L_N(128) }, { "Control_First", S(50) },
    { "Control_Default", S_N(50) }, { "Control_Cancel", S_N(50) } };
static const rp_msi_wcolumn_t control_cols[] = { { "Dialog_", KEY_S(72) }, { "Control", KEY_S(50) }, { "Type", S(20) },
    { "X", I2 }, { "Y", I2 }, { "Width", I2 }, { "Height", I2 }, { "Attributes", I4_N }, { "Property", S_N(72) },
    { "Text", L_N(0) }, { "Control_Next", S_N(50) }, { "Help", L_N(50) } };
static const rp_msi_wcolumn_t event_cols[] = { { "Dialog_", KEY_S(72) }, { "Control_", KEY_S(50) }, { "Event", KEY_S(50) },
    { "Argument", KEY_S(255) }, { "Condition", KEY_S_N(255) }, { "Ordering", I2_N } };
static const rp_msi_wcolumn_t condition_cols[] = { { "Dialog_", KEY_S(72) }, { "Control_", KEY_S(50) },
    { "Action", KEY_S(50) }, { "Condition", KEY_S(255) } };
static const rp_msi_wcolumn_t mapping_cols[] = { { "Dialog_", KEY_S(72) }, { "Control_", KEY_S(50) }, { "Event", KEY_S(50) },
    { "Attribute", S(50) } };
static const rp_msi_wcolumn_t style_cols[] = { { "TextStyle", KEY_S(72) }, { "FaceName", S(32) }, { "Size", I2 },
    { "Color", I4_N }, { "StyleBits", I2_N } };
static const rp_msi_wcolumn_t uitext_cols[] = { { "Key", KEY_S(72) }, { "Text", L_N(255) } };
static const rp_msi_wcolumn_t binary_cols[] = { { "Name", KEY_S(72) }, { "Data", 0x0900u } };
static const rp_msi_wcolumn_t radio_cols[] = { { "Property", KEY_S(72) }, { "Order", KEY_I2 }, { "Value", S(64) },
    { "X", I2 }, { "Y", I2 }, { "Width", I2 }, { "Height", I2 }, { "Text", L_N(0) }, { "Help", L_N(50) } };
static const rp_msi_wcolumn_t combo_cols[] = { { "Property", KEY_S(72) }, { "Order", KEY_I2 }, { "Value", S(64) },
    { "Text", L_N(64) } };

static void rows_init(rows_t *r, const char *name, const rp_msi_wcolumn_t *cols, size_t n) {
    *r = (rows_t){ .cols = cols, .ncols = n, .name = name };
}

static rp_msi_cell_t *cell(ctx_t *c, rows_t *r) {
    if (r->filled == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 256;
        rp_msi_cell_t *n = rp_mem_alloc(c->alloc, cap, sizeof *n);
        if (n == NULL) {
            c->nomem = true;
            return NULL;
        }
        if (r->filled) memcpy(n, r->cells, r->filled * sizeof *n);
        rp_mem_free(c->alloc, r->cells);
        r->cells = n;
        r->cap = cap;
    }
    rp_msi_cell_t *x = &r->cells[r->filled++];
    memset(x, 0, sizeof *x);
    return x;
}

static void s_(ctx_t *c, rows_t *r, const char *s) {
    rp_msi_cell_t *x = cell(c, r);
    if (x && s) *x = (rp_msi_cell_t){ .kind = RP_MSI_STR, .bytes = (const uint8_t *)s, .len = strlen(s) };
}

static void i_(ctx_t *c, rows_t *r, int32_t v) {
    rp_msi_cell_t *x = cell(c, r);
    if (x) *x = (rp_msi_cell_t){ .kind = RP_MSI_INT, .i = v };
}

static void n_(ctx_t *c, rows_t *r) { (void)cell(c, r); }

static const char *keep(ctx_t *c, const char *a, const char *b) {
    size_t n = strlen(a) + (b ? strlen(b) : 0) + 1;
    char *s = rp_mem_alloc(c->alloc, n, 1);
    if (s == NULL) {
        c->nomem = true;
        return "";
    }
    snprintf(s, n, "%s%s", a, b ? b : "");
    if (c->nstr == c->capstr) {
        size_t cap = c->capstr ? c->capstr * 2 : 128;
        char **v = rp_mem_alloc(c->alloc, cap, sizeof *v);
        if (v == NULL) {
            rp_mem_free(c->alloc, s);
            c->nomem = true;
            return "";
        }
        if (c->nstr) memcpy(v, c->strings, c->nstr * sizeof *v);
        rp_mem_free(c->alloc, c->strings);
        c->strings = v;
        c->capstr = cap;
    }
    c->strings[c->nstr++] = s;
    return s;
}

// A text in the package language, or the source's [ui-text.ID] override.
static const char *T(ctx_t *c, const char *id) {
    for (size_t i = 0; i < c->ir->ui_text_count; ++i) {
        if (strcmp(c->ir->ui_texts[i].id, id) == 0) return c->ir->ui_texts[i].text;
    }
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        if (strcmp(texts[i].id, id) == 0) return c->ko ? texts[i].ko : texts[i].en;
    }
    return id;
}

// ---- building blocks ------------------------------------------------------------------------------

enum { VIS = 1, EN = 2, SUNKEN = 4, INDIRECT = 8, TRANSPARENT = 0x10000, NOPREFIX = 0x20000,
       FIXEDSIZE = 0x100000, ICON32 = 0x400000 };

static void dialog(ctx_t *c, const char *name, int w, int h, int attrs, const char *first, const char *def,
                   const char *cancel) {
    rows_t *r = &c->dialog;
    s_(c, r, name); i_(c, r, 50); i_(c, r, 50); i_(c, r, w); i_(c, r, h); i_(c, r, attrs);
    s_(c, r, "[ProductName]"); s_(c, r, first); s_(c, r, def); s_(c, r, cancel);
}

static void control(ctx_t *c, const char *dlg, const char *name, const char *type, int x, int y, int w, int h,
                    int attrs, const char *prop, const char *text, const char *next) {
    rows_t *r = &c->control;
    s_(c, r, dlg); s_(c, r, name); s_(c, r, type); i_(c, r, x); i_(c, r, y); i_(c, r, w); i_(c, r, h); i_(c, r, attrs);
    s_(c, r, prop); s_(c, r, text); s_(c, r, next); n_(c, r);
}

static void event(ctx_t *c, const char *dlg, const char *ctl, const char *ev, const char *arg, const char *cond, int order) {
    rows_t *r = &c->event;
    s_(c, r, dlg); s_(c, r, ctl); s_(c, r, ev); s_(c, r, arg); s_(c, r, cond ? cond : "1"); i_(c, r, order);
}

static void cond(ctx_t *c, const char *dlg, const char *ctl, const char *action, const char *expr) {
    rows_t *r = &c->condition;
    s_(c, r, dlg); s_(c, r, ctl); s_(c, r, action); s_(c, r, expr);
}

static void mapping(ctx_t *c, const char *dlg, const char *ctl, const char *ev, const char *attr) {
    rows_t *r = &c->mapping;
    s_(c, r, dlg); s_(c, r, ctl); s_(c, r, ev); s_(c, r, attr);
}

// Banner (white fill, title, description) and the bottom line, with the texts as given.
static void frame_text(ctx_t *c, const char *dlg, const char *title, const char *text) {
    control(c, dlg, "Banner", "Bitmap", 0, 0, 370, 44, VIS, NULL, "RpBanner", NULL);
    control(c, dlg, "Title", "Text", 15, 7, 330, 15, VIS | TRANSPARENT | NOPREFIX, NULL, keep(c, "{\\RpTitle}", title), NULL);
    control(c, dlg, "Description", "Text", 25, 22, 330, 20, VIS | TRANSPARENT | NOPREFIX, NULL, text ? text : "", NULL);
    control(c, dlg, "BannerLine", "Line", 0, 44, 370, 0, VIS, NULL, NULL, NULL);
    control(c, dlg, "BottomLine", "Line", 0, 234, 370, 0, VIS, NULL, NULL, NULL);
}

// The same with text IDs (`text` may be NULL).
static void frame(ctx_t *c, const char *dlg, const char *title, const char *text, const char *first) {
    frame_text(c, dlg, T(c, title), text ? T(c, text) : NULL);
    (void)first;
}

// Back / Next / Cancel. `back`, `next`: the dialogs they go to (NULL: disabled; next "" = Return).
// The tab order (Control_Next) must be ONE cycle through every control that has a next pointer
// (error 2810 otherwise, observed): `first` is the dialog's first own control, whose chain ends at
// Back; Cancel closes the cycle back to it.
static void buttons(ctx_t *c, const char *dlg, const char *back, const char *next, const char *next_text,
                    const char *next_cond_disable, const char *first) {
    control(c, dlg, "Back", "PushButton", 180, 243, 56, 17, back ? VIS | EN : VIS, NULL, T(c, "Back"), "Next");
    control(c, dlg, "Next", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, next_text ? next_text : "Next"), "Cancel");
    control(c, dlg, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), first ? first : "Back");
    if (back) event(c, dlg, "Back", "NewDialog", back, NULL, 1);
    if (next && next[0]) event(c, dlg, "Next", "NewDialog", next, next_cond_disable ? keep(c, "NOT (", keep(c, next_cond_disable, ")")) : NULL, 1);
    else event(c, dlg, "Next", "EndDialog", "Return", next_cond_disable ? keep(c, "NOT (", keep(c, next_cond_disable, ")")) : NULL, 1);
    if (next_cond_disable) {
        cond(c, dlg, "Next", "Disable", next_cond_disable);
        cond(c, dlg, "Next", "Enable", keep(c, "NOT (", keep(c, next_cond_disable, ")")));
    }
    event(c, dlg, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
}

// ---- the dialogs -------------------------------------------------------------------------------

static void cancel_dlg(ctx_t *c) {
    dialog(c, "RpCancelDlg", 260, 85, 3, "No", "No", "No");
    control(c, "RpCancelDlg", "Text", "Text", 15, 15, 230, 30, VIS | NOPREFIX, NULL, T(c, "CancelText"), NULL);
    control(c, "RpCancelDlg", "Yes", "PushButton", 72, 57, 56, 17, VIS | EN, NULL, T(c, "Yes"), "No");
    control(c, "RpCancelDlg", "No", "PushButton", 132, 57, 56, 17, VIS | EN, NULL, T(c, "No"), "Yes");
    event(c, "RpCancelDlg", "Yes", "EndDialog", "Exit", NULL, 1);
    event(c, "RpCancelDlg", "No", "EndDialog", "Return", NULL, 1);
}

static void error_dlg(ctx_t *c) {
    // The engine fills ErrorText and shows the buttons the message needs (Error dialog attribute).
    dialog(c, "RpErrorDlg", 270, 105, 0x10000 | 7, "ErrorText", NULL, NULL);
    // The engine requires both ErrorText and ErrorIcon on the error dialog (error 2835, observed).
    control(c, "RpErrorDlg", "ErrorIcon", "Icon", 15, 15, 24, 24, VIS | FIXEDSIZE | ICON32, NULL, "RpWarnIcon", NULL);
    control(c, "RpErrorDlg", "ErrorText", "Text", 48, 15, 207, 55, VIS | NOPREFIX, NULL, "", NULL);
    static const char *const btn[][2] = { { "A", "ErrorAbort" }, { "C", "ErrorCancel" }, { "I", "ErrorIgnore" },
                                          { "N", "ErrorNo" }, { "O", "ErrorOk" }, { "R", "ErrorRetry" }, { "Y", "ErrorYes" } };
    static const char *const label[] = { "Abort", "Cancel", "Ignore", "No", "OK", "Retry", "Yes" };
    for (int i = 0; i < 7; ++i) {
        control(c, "RpErrorDlg", btn[i][0], "PushButton", 107, 80, 56, 17, VIS | EN, NULL, T(c, label[i]), NULL);
        event(c, "RpErrorDlg", btn[i][0], "EndDialog", btn[i][1], NULL, 1);
    }
    c->props[c->nprops++] = (rp_ui_prop_t){ "ErrorDialog", "RpErrorDlg" };
}

static void files_in_use_dlg(ctx_t *c) {
    const char *d = "FilesInUse";           // the engine looks this dialog up by name
    dialog(c, d, 370, 270, 3 | 32, "Retry", "Retry", "Exit");
    frame(c, d, "FilesInUseTitle", "FilesInUseText", NULL);
    control(c, d, "List", "ListBox", 20, 60, 330, 160, VIS | SUNKEN, "FileInUseProcess", NULL, NULL);
    control(c, d, "Retry", "PushButton", 180, 243, 56, 17, VIS | EN, NULL, T(c, "Retry"), "Ignore");
    control(c, d, "Ignore", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Ignore"), "Exit");
    control(c, d, "Exit", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Exit"), "Retry");
    event(c, d, "Retry", "EndDialog", "Retry", NULL, 1);
    event(c, d, "Ignore", "EndDialog", "Ignore", NULL, 1);
    event(c, d, "Exit", "EndDialog", "Exit", NULL, 1);
}

// Progress (modeless) and the three exit dialogs.
static void progress_and_exits(ctx_t *c) {
    const char *d = "RpProgressDlg";
    dialog(c, d, 370, 270, 1, "Cancel", "Cancel", "Cancel");
    frame(c, d, "ProgressTitle", "ProgressText", NULL);
    control(c, d, "StatusLabel", "Text", 25, 100, 50, 10, VIS | NOPREFIX, NULL, T(c, "ProgressStatus"), NULL);
    control(c, d, "ActionText", "Text", 75, 100, 270, 10, VIS | NOPREFIX, NULL, "", NULL);
    control(c, d, "ProgressBar", "ProgressBar", 25, 115, 320, 10, VIS | 0x10000, NULL, "", NULL);
    control(c, d, "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), NULL);
    control(c, d, "Next", "PushButton", 236, 243, 56, 17, VIS, NULL, T(c, "Next"), NULL);
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), NULL);
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    mapping(c, d, "ActionText", "ActionText", "Text");
    mapping(c, d, "ProgressBar", "SetProgress", "Progress");
    static const char *const exits[][3] = { { "RpExitDlg", "ExitTitle", "ExitText" },
                                            { "RpUserExitDlg", "UserExitTitle", "UserExitText" },
                                            { "RpFatalDlg", "FatalTitle", "FatalText" } };
    for (int i = 0; i < 3; ++i) {
        dialog(c, exits[i][0], 370, 270, 3, "Finish", "Finish", "Finish");
        frame(c, exits[i][0], exits[i][1], exits[i][2], NULL);
        control(c, exits[i][0], "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), NULL);
        control(c, exits[i][0], "Finish", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Finish"), NULL);
        control(c, exits[i][0], "Cancel", "PushButton", 304, 243, 56, 17, VIS, NULL, T(c, "Cancel"), NULL);
        event(c, exits[i][0], "Finish", "EndDialog", "Return", NULL, 1);
    }
    c->seqs[c->nseqs++] = (rp_ui_seq_t){ "RpExitDlg", NULL, -1 };
    c->seqs[c->nseqs++] = (rp_ui_seq_t){ "RpUserExitDlg", NULL, -2 };
    c->seqs[c->nseqs++] = (rp_ui_seq_t){ "RpFatalDlg", NULL, -3 };
    c->seqs[c->nseqs++] = (rp_ui_seq_t){ "RpProgressDlg", NULL, 1280 };
}

static void maintenance_dlg(ctx_t *c) {
    const char *d = "RpMaintenanceDlg";
    dialog(c, d, 370, 270, 3, "Repair", "Repair", "Cancel");
    frame(c, d, "MaintTitle", "MaintText", NULL);
    control(c, d, "Repair", "PushButton", 25, 65, 80, 17, VIS | EN, NULL, T(c, "Repair"), "Remove");
    control(c, d, "RepairText", "Text", 115, 67, 235, 20, VIS | NOPREFIX, NULL, T(c, "RepairText"), NULL);
    control(c, d, "Remove", "PushButton", 25, 105, 80, 17, VIS | EN, NULL, T(c, "Remove"), "Cancel");
    control(c, d, "RemoveText", "Text", 115, 107, 235, 20, VIS | NOPREFIX, NULL, T(c, "RemoveText"), NULL);
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "Repair");
    event(c, d, "Repair", "Reinstall", "ALL", NULL, 1);
    event(c, d, "Repair", "ReinstallMode", "ecmus", NULL, 2);
    event(c, d, "Repair", "EndDialog", "Return", NULL, 3);
    event(c, d, "Remove", "Remove", "ALL", NULL, 1);
    event(c, d, "Remove", "EndDialog", "Return", NULL, 2);
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    c->seqs[c->nseqs++] = (rp_ui_seq_t){ d, "Installed AND NOT RESUME AND NOT Preselected", 1240 };
}

static void welcome_dlg(ctx_t *c, const char *next, const char *next_text) {
    const char *d = "RpWelcomeDlg";
    dialog(c, d, 370, 270, 3, "Next", "Next", "Cancel");
    frame(c, d, "WelcomeTitle", NULL, NULL);
    control(c, d, "Body", "Text", 25, 60, 320, 100, VIS | NOPREFIX, NULL, T(c, "WelcomeText"), NULL);
    buttons(c, d, NULL, next, next_text, NULL, NULL);
    c->seqs[c->nseqs++] = (rp_ui_seq_t){ d, "NOT Installed", 1230 };
}

static void license_dlg(ctx_t *c, const char *rtf, const char *back, const char *next, const char *next_text) {
    const char *d = "RpLicenseDlg";
    dialog(c, d, 370, 270, 3, "LicenseText", "Next", "Cancel");
    frame(c, d, "LicenseTitle", "LicenseText", NULL);
    control(c, d, "LicenseText", "ScrollableText", 20, 55, 330, 145, VIS | SUNKEN, NULL, rtf, "Accept");
    control(c, d, "Accept", "CheckBox", 20, 207, 330, 18, VIS | EN, "RpLicenseAccepted", T(c, "LicenseAccept"), "Back");
    buttons(c, d, back, next, next_text, "RpLicenseAccepted <> \"1\"", "LicenseText");
    c->props[c->nprops++] = (rp_ui_prop_t){ "RpLicenseAccepted", NULL };    // unset until the box is ticked
}

static void installdir_dlg(ctx_t *c, const char *back, const char *next, const char *dir) {
    const char *d = "RpInstallDirDlg";
    dialog(c, d, 370, 270, 3 | 32, "Path", "Next", "Cancel");
    frame(c, d, "DirTitle", "DirText", NULL);
    control(c, d, "Label", "Text", 20, 60, 330, 12, VIS, NULL, T(c, "DirLabel"), NULL);
    control(c, d, "Path", "PathEdit", 20, 75, 330, 18, VIS | EN | SUNKEN, dir, NULL, "Browse");
    control(c, d, "Browse", "PushButton", 20, 98, 70, 17, VIS | EN, NULL, T(c, "Browse"), "Back");
    event(c, d, "Browse", "[_RpBrowseProperty]", dir, NULL, 1);
    event(c, d, "Browse", "SpawnDialog", "RpBrowseDlg", NULL, 2);
    buttons(c, d, back, next, NULL, NULL, "Path");
    // Next first commits the path (SetTargetPath checks it), then moves on.
    event(c, d, "Next", "SetTargetPath", dir, NULL, 0);

    const char *b = "RpBrowseDlg";
    dialog(c, b, 370, 270, 3, "PathEdit", "OK", "Cancel");
    frame(c, b, "BrowseTitle", "BrowseText", NULL);
    control(c, b, "LookIn", "Text", 20, 58, 50, 12, VIS, NULL, T(c, "BrowseLookIn"), NULL);
    control(c, b, "Combo", "DirectoryCombo", 72, 55, 220, 80, VIS | EN | INDIRECT | 0x70000 | 0x100000, "_RpBrowseProperty", NULL, "Up");
    control(c, b, "Up", "PushButton", 298, 55, 25, 17, VIS | EN, NULL, T(c, "BrowseUp"), "NewFolder");
    control(c, b, "NewFolder", "PushButton", 325, 55, 30, 17, VIS | EN, NULL, T(c, "BrowseNew"), "List");
    control(c, b, "List", "DirectoryList", 20, 78, 335, 110, VIS | EN | SUNKEN | INDIRECT, "_RpBrowseProperty", NULL, "PathEdit");
    control(c, b, "PathLabel", "Text", 20, 196, 60, 12, VIS, NULL, T(c, "BrowseFolder"), NULL);
    control(c, b, "PathEdit", "PathEdit", 82, 193, 273, 18, VIS | EN | SUNKEN | INDIRECT, "_RpBrowseProperty", NULL, "OK");
    control(c, b, "OK", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "OK"), "Cancel");
    control(c, b, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "Combo");
    event(c, b, "Up", "DirectoryListUp", "0", NULL, 1);
    event(c, b, "NewFolder", "DirectoryListNew", "0", NULL, 1);
    event(c, b, "OK", "SetTargetPath", "[_RpBrowseProperty]", NULL, 1);
    event(c, b, "OK", "EndDialog", "Return", NULL, 2);
    event(c, b, "Cancel", "Reset", "0", NULL, 1);
    event(c, b, "Cancel", "EndDialog", "Return", NULL, 2);
}

static void customize_dlg(ctx_t *c, const char *back, const char *next) {
    const char *d = "RpCustomizeDlg";
    dialog(c, d, 370, 270, 3 | 32, "Tree", "Next", "Cancel");
    frame(c, d, "CustomizeTitle", "CustomizeText", NULL);
    control(c, d, "Tree", "SelectionTree", 20, 55, 180, 150, VIS | EN | SUNKEN, "_RpBrowseProperty", NULL, "Reset");
    control(c, d, "ItemDescription", "Text", 210, 55, 140, 60, VIS | NOPREFIX, NULL, "", NULL);
    control(c, d, "ItemSize", "Text", 210, 120, 140, 60, VIS | NOPREFIX, NULL, "", NULL);
    control(c, d, "Reset", "PushButton", 20, 211, 70, 17, VIS | EN, NULL, T(c, "Reset"), "DiskCost");
    control(c, d, "DiskCost", "PushButton", 95, 211, 70, 17, VIS | EN, NULL, T(c, "DiskCost"), "Back");
    mapping(c, d, "ItemDescription", "SelectionDescription", "Text");
    mapping(c, d, "ItemSize", "SelectionSize", "Text");
    event(c, d, "Reset", "Reset", "0", NULL, 1);
    event(c, d, "DiskCost", "SpawnDialog", "RpDiskCostDlg", NULL, 1);
    buttons(c, d, back, next, NULL, NULL, "Tree");

    const char *k = "RpDiskCostDlg";
    dialog(c, k, 370, 270, 3 | 32, "OK", "OK", "OK");
    frame(c, k, "DiskCostTitle", "DiskCostText", NULL);
    control(c, k, "List", "VolumeCostList", 20, 55, 330, 160, VIS | SUNKEN | 0x20000, NULL,
            "{120}{70}{70}{70}{70}", "OK");
    control(c, k, "OK", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "OK"), "List");
    event(c, k, "OK", "EndDialog", "Return", NULL, 1);
}

static void ready_dlg(ctx_t *c, const char *back) {
    const char *d = "RpReadyDlg";
    dialog(c, d, 370, 270, 3 | 32, "Next", "Next", "Cancel");
    frame(c, d, "ReadyTitle", "ReadyText", NULL);
    buttons(c, d, back, "", "Install", NULL, NULL);
    // Not enough space: say so instead of starting.
    event(c, d, "Next", "SpawnDialog", "RpOutOfDiskDlg", "OutOfDiskSpace = 1", 2);
    const char *o = "RpOutOfDiskDlg";
    dialog(c, o, 370, 270, 3 | 32, "OK", "OK", "OK");
    frame(c, o, "OutOfDiskTitle", "OutOfDiskText", NULL);
    control(c, o, "List", "VolumeCostList", 20, 55, 330, 160, VIS | SUNKEN | 0x20000, NULL, "{120}{70}{70}{70}{70}", "OK");
    control(c, o, "OK", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "OK"), "List");
    event(c, o, "OK", "EndDialog", "Return", NULL, 1);
}

// An author's page (RFC-0005 K4): the built-in frame and buttons around the source's controls.
// Tab order follows the position (top to bottom, then left to right), so it never depends on the
// order of tables in the source.
static void custom_dlg(ctx_t *c, const rp_ir_dialog_t *d, const char *back, const char *next, const char *next_text) {
    const rp_ir_t *ir = c->ir;
    const rp_ir_dialog_control_t *mine[64];
    size_t n = 0;
    for (size_t k = 0; k < ir->dialog_control_count && n < 64; ++k) {
        if (ir->dialog_controls[k].dialog && strcmp(ir->dialog_controls[k].dialog, d->id) == 0) mine[n++] = &ir->dialog_controls[k];
    }
    for (size_t i = 1; i < n; ++i) {        // controls come in ID order; stable by position
        for (size_t j = i; j > 0 && (mine[j - 1]->y > mine[j]->y || (mine[j - 1]->y == mine[j]->y && mine[j - 1]->x > mine[j]->x)); --j) {
            const rp_ir_dialog_control_t *t = mine[j];
            mine[j] = mine[j - 1];
            mine[j - 1] = t;
        }
    }
    const char *first = NULL;
    for (size_t i = 0; i < n && first == NULL; ++i) {
        if (mine[i]->type != RP_DC_TEXT) first = mine[i]->id;
    }
    dialog(c, d->id, 370, 270, 3, first ? first : "Next", "Next", "Cancel");
    frame_text(c, d->id, d->title ? d->title : "[ProductName]", d->description);
    for (size_t i = 0; i < n; ++i) {
        const rp_ir_dialog_control_t *x = mine[i];
        const char *to = NULL;              // the next tab stop, or Back after the last
        if (x->type != RP_DC_TEXT) {
            to = "Back";
            for (size_t j = i + 1; j < n && strcmp(to, "Back") == 0; ++j) {
                if (mine[j]->type != RP_DC_TEXT) to = mine[j]->id;
            }
        }
        switch (x->type) {
        case RP_DC_TEXT:
            control(c, d->id, x->id, "Text", x->x, x->y, x->width, x->height, VIS | TRANSPARENT, NULL, x->text, NULL);
            break;
        case RP_DC_CHECKBOX:        // ticked: the property is "1"; clear: the property is removed
            control(c, d->id, x->id, "CheckBox", x->x, x->y, x->width, x->height, VIS | EN, x->property, x->text, to);
            break;
        case RP_DC_EDIT:
            control(c, d->id, x->id, "Edit", x->x, x->y, x->width, x->height, VIS | EN | SUNKEN, x->property, NULL, to);
            break;
        case RP_DC_RADIO: {
            control(c, d->id, x->id, "RadioButtonGroup", x->x, x->y, x->width, x->height, VIS | EN, x->property, NULL, to);
            int step = x->value_count ? x->height / (int)x->value_count : x->height;
            for (size_t j = 0; j < x->value_count; ++j) {
                rows_t *r = &c->radio;
                s_(c, r, x->property); i_(c, r, (int32_t)j + 1); s_(c, r, x->values[j]); i_(c, r, 0);
                i_(c, r, (int32_t)j * step); i_(c, r, x->width); i_(c, r, step < 14 ? step : 14); s_(c, r, x->labels[j]); n_(c, r);
            }
            break;
        }
        case RP_DC_COMBO:           // a drop-down list (0x20000): one of the values, nothing typed. Without
                                    // Sorted (0x10000) the engine lists them alphabetically (observed).
            control(c, d->id, x->id, "ComboBox", x->x, x->y, x->width, x->height, VIS | EN | SUNKEN | 0x20000 | 0x10000,
                    x->property, NULL, to);
            for (size_t j = 0; j < x->value_count; ++j) {
                rows_t *r = &c->combo;
                s_(c, r, x->property); i_(c, r, (int32_t)j + 1); s_(c, r, x->values[j]); s_(c, r, x->labels[j]);
            }
            break;
        default:
            break;
        }
    }
    buttons(c, d->id, back, next, next_text, NULL, first);
}

typedef struct {
    const char           *name;
    const rp_ir_dialog_t *custom;   // NULL for a built-in page
} page_t;

// Appends the author's pages placed after `anchor` (in ID order), each followed by its own.
static void place(const rp_ir_t *ir, page_t *pages, size_t *n, size_t cap, const char *anchor, int depth) {
    for (size_t k = 0; k < ir->dialog_count && depth <= (int)ir->dialog_count; ++k) {
        const rp_ir_dialog_t *d = &ir->dialogs[k];
        if (d->after && strcmp(d->after, anchor) == 0 && *n < cap) {
            pages[(*n)++] = (page_t){ d->id, d };
            place(ir, pages, n, cap, d->id, depth + 1);
        }
    }
}

// ---- entry -------------------------------------------------------------------------------------

static void white_bmp(uint8_t out[58]) {
    // 1 x 1, 24 bits, white: 14-byte file header, 40-byte info header, 4 bytes of pixel row.
    static const uint8_t bmp[58] = { 'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
                                     1, 0, 24, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(out, bmp, sizeof bmp);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// The warning icon of the error dialog (K1: no bitmaps of anyone else's): an amber disc with a
// white exclamation mark, drawn here.
static void warn_icon(uint8_t out[ICON_SIZE]) {
    memset(out, 0, ICON_SIZE);
    put16(out + 2, 1);                                  // type: icon
    put16(out + 4, 1);                                  // one image
    uint8_t *e = out + 6;
    e[0] = 32; e[1] = 32;                               // width, height
    put16(e + 4, 1); put16(e + 6, 32);                  // planes, bits
    put32(e + 8, 40 + ICON_PIXELS + 32 * 4);            // image size
    put32(e + 12, 6 + 16);                              // image offset
    uint8_t *h = out + 22;
    put32(h, 40); put32(h + 4, 32); put32(h + 8, 64);   // header size, width, height x 2 (image + mask)
    put16(h + 12, 1); put16(h + 14, 32);
    put32(h + 20, ICON_PIXELS + 32 * 4);
    uint8_t *px = h + 40;
    for (int y = 0; y < 32; ++y) {                      // y from the top; BMP rows go bottom up
        for (int x = 0; x < 32; ++x) {
            int dx = 2 * x - 31, dy = 2 * y - 31;
            uint8_t *q = px + ((31 - y) * 32 + x) * 4;
            if (dx * dx + dy * dy > 29 * 29) continue;  // outside the disc: transparent
            bool mark = x >= 14 && x <= 17 && ((y >= 6 && y <= 19) || (y >= 22 && y <= 25));
            q[0] = mark ? 0xFF : 0x00; q[1] = mark ? 0xFF : 0xA0; q[2] = mark ? 0xFF : 0xF0; q[3] = 0xFF;
        }
    }
}

static const char *cell_str(const rp_msi_cell_t *x, char *buf, size_t cap) {
    if (x->kind != RP_MSI_STR || x->len + 1 > cap) return NULL;
    memcpy(buf, x->bytes, x->len);
    buf[x->len] = 0;
    return buf;
}

// The engine refuses a dialog whose tab order (Control_Next) is not one cycle through every control
// that has a next pointer, starting at Control_First (errors 2809, 2810 - both seen on Windows), or
// whose Control_First / Default / Cancel is not one of its controls. Checked on every build so a
// broken set never reaches a package.
static bool check_tab_order(const ctx_t *c) {
    const rows_t *d = &c->dialog, *k = &c->control;
    size_t nd = d->filled / d->ncols, nk = k->filled / k->ncols;
    char a[96], b[96], e[96];
    for (size_t i = 0; i < nd; ++i) {
        const rp_msi_cell_t *dr = d->cells + i * d->ncols;
        const char *dn = cell_str(&dr[0], a, sizeof a);
        if (dn == NULL) return false;
        size_t with_next = 0;
        for (size_t col = 7; col <= 9; ++col) {         // Control_First, _Default, _Cancel exist
            const char *want = dr[col].kind == RP_MSI_STR ? cell_str(&dr[col], e, sizeof e) : NULL;
            if (want == NULL) continue;
            bool found = false;
            for (size_t j = 0; j < nk && !found; ++j) {
                const rp_msi_cell_t *kr = k->cells + j * k->ncols;
                found = strcmp(cell_str(&kr[0], b, sizeof b) ? b : "", dn) == 0 && cell_str(&kr[1], b, sizeof b) && strcmp(b, want) == 0;
            }
            if (!found) return false;
        }
        for (size_t j = 0; j < nk; ++j) {
            const rp_msi_cell_t *kr = k->cells + j * k->ncols;
            if (cell_str(&kr[0], b, sizeof b) && strcmp(b, dn) == 0 && kr[10].kind == RP_MSI_STR) ++with_next;
        }
        if (with_next == 0) continue;
        // Walk from Control_First.
        char cur[96];
        if (cell_str(&dr[7], cur, sizeof cur) == NULL) return false;
        char first[96];
        memcpy(first, cur, sizeof cur);
        bool closed = false;
        for (size_t step = 0; step < with_next && !closed; ++step) {
            const rp_msi_cell_t *hit = NULL;
            for (size_t j = 0; j < nk && hit == NULL; ++j) {
                const rp_msi_cell_t *kr = k->cells + j * k->ncols;
                if (cell_str(&kr[0], b, sizeof b) && strcmp(b, dn) == 0 && cell_str(&kr[1], b, sizeof b) && strcmp(b, cur) == 0)
                    hit = kr;
            }
            if (hit == NULL || cell_str(&hit[10], cur, sizeof cur) == NULL) return false;
            if (strcmp(cur, first) == 0) {
                if (step + 1 != with_next) return false;    // a shorter cycle leaves controls out
                closed = true;
            }
        }
        if (!closed) return false;
    }
    return true;
}

proven_err_t rp_ui_build(proven_allocator_t alloc, const rp_ir_t *ir, const rp_ui_input_t *in, rp_ui_t **out) {
    if (ir == NULL || in == NULL || out == NULL) return PROVEN_ERR_INVALID_ARG;
    *out = NULL;
    ctx_t *c = rp_mem_alloc(alloc, 1, sizeof *c);
    rp_ui_t *ui = rp_mem_alloc(alloc, 1, sizeof *ui);
    rp_msi_wtable_t *tables = rp_mem_alloc(alloc, 10, sizeof *tables);
    if (c == NULL || ui == NULL || tables == NULL) {
        rp_mem_free(alloc, c);
        rp_mem_free(alloc, ui);
        rp_mem_free(alloc, tables);
        return PROVEN_ERR_NOMEM;
    }
    memset(c, 0, sizeof *c);
    c->alloc = alloc;
    c->ir = ir;
    c->ko = ir->language == 1042;
    rows_init(&c->dialog, "Dialog", dialog_cols, 10);
    rows_init(&c->control, "Control", control_cols, 12);
    rows_init(&c->event, "ControlEvent", event_cols, 6);
    rows_init(&c->condition, "ControlCondition", condition_cols, 4);
    rows_init(&c->mapping, "EventMapping", mapping_cols, 4);
    rows_init(&c->style, "TextStyle", style_cols, 5);
    rows_init(&c->uitext, "UIText", uitext_cols, 2);
    rows_init(&c->binary, "Binary", binary_cols, 2);
    rows_init(&c->radio, "RadioButton", radio_cols, 9);
    rows_init(&c->combo, "ComboBox", combo_cols, 4);

    // Fonts: the Korean face for Korean text (P1a: Hangul shows in these faces), Segoe UI otherwise.
    const char *face = c->ko ? "맑은 고딕" : "Segoe UI";
    s_(c, &c->style, "RpNormal"); s_(c, &c->style, face); i_(c, &c->style, 9); n_(c, &c->style); n_(c, &c->style);
    s_(c, &c->style, "RpTitle"); s_(c, &c->style, face); i_(c, &c->style, 11); n_(c, &c->style); i_(c, &c->style, 1);
    c->props[c->nprops++] = (rp_ui_prop_t){ "DefaultUIFont", "RpNormal" };
    for (size_t i = 0; i < sizeof uitexts / sizeof uitexts[0]; ++i) {
        s_(c, &c->uitext, uitexts[i].id); s_(c, &c->uitext, c->ko ? uitexts[i].ko : uitexts[i].en);
    }
    white_bmp(c->white);
    rp_msi_cell_t *bn = NULL;
    s_(c, &c->binary, "RpBanner");
    bn = cell(c, &c->binary);
    if (bn) {
        bn->kind = RP_MSI_BINARY;
        bn->bytes = in->banner_bmp ? in->banner_bmp : c->white;
        bn->len = in->banner_bmp ? in->banner_len : sizeof c->white;
    }

    warn_icon(c->icon);
    s_(c, &c->binary, "RpWarnIcon");
    rp_msi_cell_t *ic = cell(c, &c->binary);
    if (ic) *ic = (rp_msi_cell_t){ .kind = RP_MSI_BINARY, .bytes = c->icon, .len = sizeof c->icon };

    cancel_dlg(c);
    error_dlg(c);
    files_in_use_dlg(c);
    progress_and_exits(c);
    const char *rtf = in->license_rtf ? keep(c, "", "") : NULL;
    if (in->license_rtf) {      // the RTF text as a string cell (it is 7-bit ASCII: \uN? escapes)
        char *t = rp_mem_alloc(alloc, in->license_len + 1, 1);
        if (t) {
            memcpy(t, in->license_rtf, in->license_len);
            t[in->license_len] = 0;
            rtf = keep(c, t, NULL);
            rp_mem_free(alloc, t);
        }
    }
    const char *dir = in->install_dir ? in->install_dir : "TARGETDIR";
    if (ir->ui >= RP_UI_MINIMAL) {
        // The pages in order: the set's own, each followed by the author's pages placed after it.
        size_t cap = 5 + ir->dialog_count, n = 0;
        page_t *pages = rp_mem_alloc(alloc, cap, sizeof *pages);
        if (pages == NULL) c->nomem = true;
        const char *base[5];
        size_t nb = 0;
        base[nb++] = "RpWelcomeDlg";
        if (rtf) base[nb++] = "RpLicenseDlg";
        if (ir->ui >= RP_UI_INSTALLDIR) base[nb++] = "RpInstallDirDlg";
        if (ir->ui == RP_UI_FEATURES) base[nb++] = "RpCustomizeDlg";
        if (ir->ui >= RP_UI_INSTALLDIR) base[nb++] = "RpReadyDlg";
        for (size_t i = 0; pages && i < nb; ++i) {
            pages[n++] = (page_t){ base[i], NULL };
            place(ir, pages, &n, cap, base[i], 0);
        }
        bool ready = ir->ui >= RP_UI_INSTALLDIR;    // minimal: the last page's Next starts the installation
        for (size_t i = 0; pages && i < n; ++i) {
            const char *back = i ? pages[i - 1].name : NULL;
            const char *next = i + 1 < n ? pages[i + 1].name : "";
            const char *next_text = !ready && i + 1 == n ? "Install" : NULL;
            const char *p = pages[i].name;
            if (pages[i].custom) custom_dlg(c, pages[i].custom, back, next, next_text);
            else if (strcmp(p, "RpWelcomeDlg") == 0) welcome_dlg(c, next, next_text);
            else if (strcmp(p, "RpLicenseDlg") == 0) license_dlg(c, rtf, back, next, next_text);
            else if (strcmp(p, "RpInstallDirDlg") == 0) installdir_dlg(c, back, next, dir);
            else if (strcmp(p, "RpCustomizeDlg") == 0) customize_dlg(c, back, next);
            else ready_dlg(c, back);
        }
        rp_mem_free(alloc, pages);
        maintenance_dlg(c);
        if (ir->ui >= RP_UI_INSTALLDIR) c->props[c->nprops++] = (rp_ui_prop_t){ "_RpBrowseProperty", dir };
    }

    rows_t *all[] = { &c->dialog, &c->control, &c->event, &c->condition, &c->mapping, &c->style, &c->uitext, &c->binary, &c->radio, &c->combo };
    size_t nt = 0;
    for (size_t i = 0; i < 10; ++i) {
        rows_t *r = all[i];
        if (r->filled == 0) continue;
        tables[nt++] = (rp_msi_wtable_t){ r->name, r->cols, r->ncols, r->cells, r->filled / r->ncols };
    }
    ui->tables = tables;
    ui->table_count = nt;
    ui->props = c->props;
    ui->prop_count = c->nprops;
    ui->seqs = c->seqs;
    ui->seq_count = c->nseqs;
    ui->priv = c;
    if (c->nomem) {
        rp_ui_free(alloc, ui);
        return PROVEN_ERR_NOMEM;
    }
    if (!check_tab_order(c)) {
        rp_ui_free(alloc, ui);
        return PROVEN_ERR_INVALID_STATE;
    }
    *out = ui;
    return PROVEN_OK;
}

void rp_ui_free(proven_allocator_t alloc, rp_ui_t *ui) {
    if (ui == NULL) return;
    ctx_t *c = ui->priv;
    if (c) {
        rows_t *all[] = { &c->dialog, &c->control, &c->event, &c->condition, &c->mapping, &c->style, &c->uitext, &c->binary, &c->radio, &c->combo };
        for (size_t i = 0; i < 10; ++i) rp_mem_free(alloc, all[i]->cells);
        for (size_t i = 0; i < c->nstr; ++i) rp_mem_free(alloc, c->strings[i]);
        rp_mem_free(alloc, c->strings);
        rp_mem_free(alloc, c);
    }
    rp_mem_free(alloc, ui->tables);
    rp_mem_free(alloc, ui);
}

// ---- license RTF -----------------------------------------------------------------------------

proven_err_t rp_ui_text_to_rtf(proven_allocator_t alloc, const uint8_t *text, size_t len, bool korean, uint8_t **out,
                               size_t *out_len) {
    size_t cap = 256 + len * 10, n = 0;
    char *s = rp_mem_alloc(alloc, cap, 1);
    if (s == NULL) return PROVEN_ERR_NOMEM;
    n += (size_t)snprintf(s, cap, "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fnil %s;}}\\fs18 ",
                          korean ? "Malgun Gothic" : "Segoe UI");
    size_t i = 0;
    if (len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) i = 3;
    while (i < len && n + 16 < cap) {
        uint32_t cp = text[i];
        size_t k = 1;
        if (cp >= 0xF0 && i + 3 < len) cp = (cp & 7) << 18 | (text[i + 1] & 63u) << 12 | (text[i + 2] & 63u) << 6 | (text[i + 3] & 63u), k = 4;
        else if (cp >= 0xE0 && i + 2 < len) cp = (cp & 15) << 12 | (text[i + 1] & 63u) << 6 | (text[i + 2] & 63u), k = 3;
        else if (cp >= 0xC0 && i + 1 < len) cp = (cp & 31) << 6 | (text[i + 1] & 63u), k = 2;
        i += k;
        if (cp == '\r') continue;
        if (cp == '\n') n += (size_t)snprintf(s + n, cap - n, "\\par\n");
        else if (cp == '\\' || cp == '{' || cp == '}') n += (size_t)snprintf(s + n, cap - n, "\\%c", (char)cp);
        else if (cp < 0x80) s[n++] = (char)cp;
        else if (cp < 0x10000) n += (size_t)snprintf(s + n, cap - n, "\\u%d?", (int)(int16_t)cp);
        else {  // a surrogate pair, each as its own \uN
            uint32_t v = cp - 0x10000;
            n += (size_t)snprintf(s + n, cap - n, "\\u%d?\\u%d?", (int)(int16_t)(0xD800 + (v >> 10)), (int)(int16_t)(0xDC00 + (v & 0x3FF)));
        }
    }
    n += (size_t)snprintf(s + n, cap - n, "}");
    *out = (uint8_t *)s;
    *out_len = n;
    return PROVEN_OK;
}
