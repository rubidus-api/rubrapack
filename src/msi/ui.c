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
    { "DirGuardText", "설치를 멈췄습니다: 폴더 [1] 이(가) 이미 있는데 관리자 소유가 아니거나 다른 곳으로 이어지는 연결입니다. 다른 폴더를 고르거나, 관리자가 먼저 그 폴더를 지우게 하십시오.",
      "Setup stopped: the folder [1] already exists and is not owned by administrators, or it leads somewhere else through a link. Choose another folder, or have an administrator remove it first." },
    { "LaunchText", "[ProductName] 실행(&L)", "&Launch [ProductName]" },
    { "Change", "변경(&C)", "&Change" },
    { "ChangeText", "설치할 기능을 바꿉니다.", "Choose which features are installed." },
    { "ScopeTitle", "설치 범위", "Installation scope" },
    { "ScopeText", "[ProductName]을(를) 누가 쓸지 고르십시오.", "Choose who can use [ProductName]." },
    { "ScopeUser", "나만(&M)", "Just &me" },
    { "ScopeMachine", "이 컴퓨터의 모든 사용자(&E) - 관리자 권한이 필요합니다", "&Everyone on this computer - needs administrator rights" },
    { "LanguageTitle", "언어", "Language" },
    { "LanguageText", "설치에 쓸 언어를 고르십시오.", "Choose the language for setup." },
};

bool rp_ui_text_known(const char *id) {
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        if (strcmp(texts[i].id, id) == 0) return true;
    }
    return false;
}

const char *rp_ui_text_id(size_t i) { return i < sizeof texts / sizeof texts[0] ? texts[i].id : NULL; }

// The properties a text may use and still be settled at build time (their values are in the source).
static const char *const static_props[] = { "ProductName", "Manufacturer", "ProductVersion" };

bool rp_ui_text_static(const char *text) {
    for (const char *p = text; *p; ++p) {
        if (*p == '{') return false;            // {...} segments are the formatter's
        if (*p != '[') continue;
        const char *e = strchr(p, ']');
        if (e == NULL) return false;
        size_t n = (size_t)(e - p - 1);
        bool known = n == 2 && p[1] == '\\';      // [\x]: a literal character
        for (size_t i = 0; i < 3 && !known; ++i) known = strlen(static_props[i]) == n && strncmp(p + 1, static_props[i], n) == 0;
        if (!known) return false;
        p = e;
    }
    return true;
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

// A text of the dialogs when there are several languages (RFC-0012): the controls show [RpT_<key>],
// which the chosen language fills.
typedef struct {
    const char *key;
    const char *src[RP_UI_LANG_MAX];    // in each language of ir->ui_langs (formatted source)
    bool        title;                  // a banner heading: the value starts with the language's title style
} regtext_t;

typedef struct {
    proven_allocator_t alloc;
    const rp_ir_t     *ir;
    bool               multi;           // more than one language
    bool               page;            // ... and the language page leads the flow
    bool               maint_back;      // buttons(): Back also leads to the maintenance page
    size_t             nlang;
    bool               nomem;
    char             **strings;
    size_t             nstr, capstr;
    rows_t             dialog, control, event, condition, mapping, style, uitext, binary, radio, combo, listbox;
    rp_ui_prop_t      *props;
    size_t             nprops, capprops;
    rp_ui_seq_t       *seqs;
    size_t             nseqs, capseqs;
    rp_ui_ca_t        *cas;
    size_t             ncas, capcas;
    regtext_t         *reg;
    size_t             nreg, capreg;
    size_t             ncustom;
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
static const rp_msi_wcolumn_t listbox_cols[] = { { "Property", KEY_S(72) }, { "Order", KEY_I2 }, { "Value", S(64) },
    { "Text", L_N(64) } };
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

static bool grow(ctx_t *c, void **p, size_t *cap, size_t n, size_t size) {
    if (n < *cap) return true;
    size_t cap2 = *cap ? *cap * 2 : 32;
    void *q = rp_mem_alloc(c->alloc, cap2, size);
    if (q == NULL) {
        c->nomem = true;
        return false;
    }
    if (n) memcpy(q, *p, n * size);
    rp_mem_free(c->alloc, *p);
    *p = q;
    *cap = cap2;
    return true;
}

static void prop(ctx_t *c, const char *name, const char *value) {
    if (grow(c, (void **)&c->props, &c->capprops, c->nprops, sizeof *c->props)) c->props[c->nprops++] = (rp_ui_prop_t){ name, value };
}

static void seq(ctx_t *c, const char *action, const char *cond, int n) {
    if (grow(c, (void **)&c->seqs, &c->capseqs, c->nseqs, sizeof *c->seqs)) c->seqs[c->nseqs++] = (rp_ui_seq_t){ action, cond, n };
}

static void ca(ctx_t *c, const char *action, const char *source, const char *target) {
    if (grow(c, (void **)&c->cas, &c->capcas, c->ncas, sizeof *c->cas)) c->cas[c->ncas++] = (rp_ui_ca_t){ action, source, target };
}

static const char *lang_code(const ctx_t *c, size_t li) { return c->ir->ui_lang_count ? c->ir->ui_langs[li].code : "en"; }

// Text `id` in language `li`: the source's [ui-text.ID] text-xx, then its text, then the built-in one
// (English and Korean; other languages give every text, which lint checks).
const char *rp_ui_text_for(const rp_ir_t *ir, const char *id, size_t li) {
    const char *code = ir->ui_lang_count ? ir->ui_langs[li].code : "en";
    for (size_t i = 0; i < ir->ui_text_count; ++i) {
        const rp_ir_ui_text_t *x = &ir->ui_texts[i];
        if (strcmp(x->id, id) != 0) continue;
        for (size_t j = 0; j < x->by_lang_count; ++j) {
            if (strcmp(x->by_lang[j].lang, code) == 0) return x->by_lang[j].text;
        }
        if (x->text) return x->text;
    }
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        if (strcmp(texts[i].id, id) == 0) return strcmp(code, "ko") == 0 ? texts[i].ko : texts[i].en;
    }
    return id;
}

static const char *text_for(const ctx_t *c, const char *id, size_t li) { return rp_ui_text_for(c->ir, id, li); }

// Registers a text shown through [RpT_<key>] and returns that reference.
static const char *reg_text(ctx_t *c, const char *key, const char *const *src, bool title) {
    for (size_t i = 0; i < c->nreg; ++i) {
        if (strcmp(c->reg[i].key, key) == 0) return keep(c, "[RpT_", keep(c, key, "]"));
    }
    if (!grow(c, (void **)&c->reg, &c->capreg, c->nreg, sizeof *c->reg)) return "";
    regtext_t *r = &c->reg[c->nreg++];
    *r = (regtext_t){ .key = key, .title = title };
    for (size_t i = 0; i < c->nlang; ++i) r->src[i] = src[i];
    return keep(c, "[RpT_", keep(c, key, "]"));
}

// A built-in text: literal with one language, a property with several.
static const char *T(ctx_t *c, const char *id) {
    if (!c->multi) return text_for(c, id, 0);
    const char *src[RP_UI_LANG_MAX] = { 0 };
    for (size_t i = 0; i < c->nlang; ++i) src[i] = text_for(c, id, i);
    return reg_text(c, id, src, false);
}

// The same for a banner heading, with its title style.
static const char *TT(ctx_t *c, const char *id) {
    if (!c->multi) return keep(c, "{\\RpTitle}", text_for(c, id, 0));
    const char *src[RP_UI_LANG_MAX] = { 0 };
    for (size_t i = 0; i < c->nlang; ++i) src[i] = text_for(c, id, i);
    return reg_text(c, id, src, true);
}

// An author's text (RFC-0012): `all` for every language, `by` for some. NULL when there is none.
static const char *AT(ctx_t *c, const char *all, const rp_ir_ltext_t *by, size_t nby, bool title) {
    if (all == NULL && nby == 0) return NULL;
    if (!c->multi) {
        const char *t = all;
        for (size_t j = 0; j < nby; ++j) {
            if (strcmp(by[j].lang, "en") == 0) t = by[j].text;
        }
        t = t ? t : "";
        return title ? keep(c, "{\\RpTitle}", t) : t;
    }
    const char *src[RP_UI_LANG_MAX] = { 0 };
    for (size_t i = 0; i < c->nlang; ++i) {
        src[i] = all ? all : "";
        for (size_t j = 0; j < nby; ++j) {
            if (strcmp(by[j].lang, lang_code(c, i)) == 0) src[i] = by[j].text;
        }
    }
    char key[24];
    snprintf(key, sizeof key, "c%zu", ++c->ncustom);
    return reg_text(c, keep(c, key, NULL), src, title);
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

// Banner (white fill, title, description) and the bottom line, with the texts as given (the title
// with its style, from TT or AT).
static void frame_text(ctx_t *c, const char *dlg, const char *title, const char *text) {
    control(c, dlg, "Banner", "Bitmap", 0, 0, 370, 44, VIS, NULL, "RpBanner", NULL);
    control(c, dlg, "Title", "Text", 15, 7, 330, 15, VIS | TRANSPARENT | NOPREFIX, NULL, title, NULL);
    control(c, dlg, "Description", "Text", 25, 22, 330, 20, VIS | TRANSPARENT | NOPREFIX, NULL, text ? text : "", NULL);
    control(c, dlg, "BannerLine", "Line", 0, 44, 370, 0, VIS, NULL, NULL, NULL);
    control(c, dlg, "BottomLine", "Line", 0, 234, 370, 0, VIS, NULL, NULL, NULL);
}

// The same with text IDs (`text` may be NULL).
static void frame(ctx_t *c, const char *dlg, const char *title, const char *text, const char *first) {
    frame_text(c, dlg, TT(c, title), text ? T(c, text) : NULL);
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
    if (back && c->maint_back) {            // the feature tree, reached from the maintenance page too
        event(c, dlg, "Back", "NewDialog", back, "NOT Installed", 1);
        event(c, dlg, "Back", "NewDialog", "RpMaintenanceDlg", "Installed", 1);
    } else if (back) {
        event(c, dlg, "Back", "NewDialog", back, NULL, 1);
    }
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
    prop(c, "ErrorDialog", "RpErrorDlg");
}

static void files_in_use_dlg(ctx_t *c) {
    const char *d = "FilesInUse";           // the engine looks this dialog up by name
    dialog(c, d, 370, 270, 3 | 32, "Retry", "Retry", "Exit");
    frame(c, d, "FilesInUseTitle", NULL, NULL);     // the text is too long for the banner (observed cut)
    control(c, d, "Text", "Text", 20, 52, 330, 30, VIS | NOPREFIX, NULL, T(c, "FilesInUseText"), NULL);
    control(c, d, "List", "ListBox", 20, 85, 330, 140, VIS | SUNKEN, "FileInUseProcess", NULL, NULL);
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
        // RFC-0013 A5: the finished page may start the program (RP_Launch, made by the lowering),
        // after a first installation or an upgrade only.
        bool launch = i == 0 && c->ir->ui_launch_file;
        dialog(c, exits[i][0], 370, 270, 3, "Finish", "Finish", "Finish");
        frame(c, exits[i][0], exits[i][1], exits[i][2], NULL);
        if (launch) {
            control(c, exits[i][0], "Launch", "CheckBox", 25, 80, 320, 17, VIS | EN, "RPLAUNCH", T(c, "LaunchText"), "Finish");
            cond(c, exits[i][0], "Launch", "Hide", "Installed");
            event(c, exits[i][0], "Finish", "DoAction", "RP_Launch", "RPLAUNCH = \"1\" AND NOT Installed", 1);
        }
        control(c, exits[i][0], "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), NULL);
        control(c, exits[i][0], "Finish", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Finish"), launch ? "Launch" : NULL);
        control(c, exits[i][0], "Cancel", "PushButton", 304, 243, 56, 17, VIS, NULL, T(c, "Cancel"), NULL);
        event(c, exits[i][0], "Finish", "EndDialog", "Return", NULL, launch ? 2 : 1);
    }
    if (c->ir->ui_launch_file && c->ir->ui_launch_default) prop(c, "RPLAUNCH", "1");
    seq(c, "RpExitDlg", NULL, -1);
    seq(c, "RpUserExitDlg", NULL, -2);
    seq(c, "RpFatalDlg", NULL, -3);
    seq(c, "RpProgressDlg", NULL, 1280);
}

// RFC-0013 A4: with the features set, Change (to the feature tree) comes first.
static void maintenance_dlg(ctx_t *c, bool change) {
    const char *d = "RpMaintenanceDlg";
    int y = change ? 40 : 0;
    dialog(c, d, 370, 270, 3, change ? "Change" : "Repair", "Repair", "Cancel");
    frame(c, d, "MaintTitle", "MaintText", NULL);
    if (change) {
        control(c, d, "Change", "PushButton", 25, 65, 80, 17, VIS | EN, NULL, T(c, "Change"), "Repair");
        control(c, d, "ChangeText", "Text", 115, 67, 235, 20, VIS | NOPREFIX, NULL, T(c, "ChangeText"), NULL);
        event(c, d, "Change", "NewDialog", "RpCustomizeDlg", NULL, 1);
    }
    control(c, d, "Repair", "PushButton", 25, 65 + y, 80, 17, VIS | EN, NULL, T(c, "Repair"), "Remove");
    control(c, d, "RepairText", "Text", 115, 67 + y, 235, 20, VIS | NOPREFIX, NULL, T(c, "RepairText"), NULL);
    control(c, d, "Remove", "PushButton", 25, 105 + y, 80, 17, VIS | EN, NULL, T(c, "Remove"), "Cancel");
    control(c, d, "RemoveText", "Text", 115, 107 + y, 235, 20, VIS | NOPREFIX, NULL, T(c, "RemoveText"), NULL);
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), change ? "Change" : "Repair");
    event(c, d, "Repair", "Reinstall", "ALL", NULL, 1);
    event(c, d, "Repair", "ReinstallMode", "ecmus", NULL, 2);
    event(c, d, "Repair", "EndDialog", "Return", NULL, 3);
    event(c, d, "Remove", "Remove", "ALL", NULL, 1);
    event(c, d, "Remove", "EndDialog", "Return", NULL, 2);
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    if (!c->page) seq(c, d, "Installed AND NOT RESUME AND NOT Preselected", 1240);
}

static void welcome_dlg(ctx_t *c, const char *next, const char *next_text) {
    const char *d = "RpWelcomeDlg";
    dialog(c, d, 370, 270, 3, "Next", "Next", "Cancel");
    frame(c, d, "WelcomeTitle", NULL, NULL);
    control(c, d, "Body", "Text", 25, 60, 320, 100, VIS | NOPREFIX, NULL, T(c, "WelcomeText"), NULL);
    buttons(c, d, c->page ? "RpLanguageDlg" : NULL, next, next_text, NULL, NULL);
    if (!c->page) seq(c, d, "NOT Installed", 1230);
}

static const char *lang_cond(ctx_t *c, size_t li);

// One license for every language, or (RFC-0012 license-xx) one ScrollableText per language in the
// same place, each shown only for its language.
static void license_dlg(ctx_t *c, const char *const *rtf, const char *back, const char *next, const char *next_text) {
    const char *d = "RpLicenseDlg";
    bool several = false;
    for (size_t i = 1; i < c->nlang; ++i) several |= strcmp(rtf[i], rtf[0]) != 0;
    size_t n = several ? c->nlang : 1;
    // The first control must be visible whatever the language (error 2836 otherwise, observed):
    // with a license per language that is the check box.
    dialog(c, d, 370, 270, 3, several ? "Accept" : "LicenseText", "Next", "Cancel");
    frame(c, d, "LicenseTitle", "LicenseText", NULL);
    for (size_t i = 0; i < n; ++i) {
        const char *name = i ? keep(c, "LicenseText_", lang_code(c, i)) : "LicenseText";
        const char *to = i + 1 < n ? keep(c, "LicenseText_", lang_code(c, i + 1)) : "Accept";
        control(c, d, name, "ScrollableText", 20, 55, 330, 145, VIS | SUNKEN, NULL, rtf[i], to);
        if (several) {
            cond(c, d, name, "Show", lang_cond(c, i));
            cond(c, d, name, "Hide", keep(c, "NOT (", keep(c, lang_cond(c, i), ")")));
        }
    }
    control(c, d, "Accept", "CheckBox", 20, 207, 330, 18, VIS | EN, "RpLicenseAccepted", T(c, "LicenseAccept"), "Back");
    buttons(c, d, back, next, next_text, "RpLicenseAccepted <> \"1\"", "LicenseText");
    prop(c, "RpLicenseAccepted", NULL);    // unset until the box is ticked
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
    c->maint_back = true;
    buttons(c, d, back, next, NULL, NULL, "Tree");
    c->maint_back = false;

    const char *k = "RpDiskCostDlg";
    dialog(c, k, 370, 270, 3 | 32, "OK", "OK", "OK");
    frame(c, k, "DiskCostTitle", "DiskCostText", NULL);
    control(c, k, "List", "VolumeCostList", 20, 55, 330, 160, VIS | SUNKEN | 0x20000, NULL,
            "{120}{70}{70}{70}{70}", "OK");
    control(c, k, "OK", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "OK"), "List");
    event(c, k, "OK", "EndDialog", "Return", NULL, 1);
}

// The folder of dir `id` for one scope (RFC-0013 A6), when it lies under ProgramFiles: the machine's
// Program Files from the environment (the folder properties follow the per-user default), or the
// user's Programs folder. NULL when the dir is elsewhere.
static const char *scope_path(ctx_t *c, const char *id, bool machine) {
    const rp_ir_t *ir = c->ir;
    const char *tail = "";
    for (int depth = 0; id && depth < 64; ++depth) {
        const rp_ir_dir_t *d = NULL;
        for (size_t i = 0; i < ir->dir_count; ++i) {
            if (strcmp(ir->dirs[i].id, id) == 0) d = &ir->dirs[i];
        }
        if (d == NULL) return NULL;
        const char *mine = "";
        for (size_t j = 0; j < d->part_count; ++j) mine = keep(c, mine, keep(c, d->parts[j], "\\"));
        tail = keep(c, mine, tail);
        if (d->base) {
            if (strcmp(d->base, "ProgramFiles") != 0) return NULL;
            const char *root = !machine ? "[LocalAppDataFolder]Programs\\"
                             : ir->arch == RP_ARCH_X86 ? "[%ProgramFiles(x86)]\\" : "[%ProgramW6432]\\";
            return keep(c, root, tail);
        }
        id = d->parent;
    }
    return NULL;
}

// "Just me" or "everyone" for a dual package (RFC-0013 A6): the scope properties, then the install
// folder moved to that scope's place (SetTargetPath takes the path from the dir's property).
static void scope_dlg(ctx_t *c, const char *back, const char *next, const char *next_text, const char *dir, const char *dir_id) {
    const char *d = "RpScopeDlg";
    dialog(c, d, 370, 270, 3, "Scope", "Next", "Cancel");
    frame(c, d, "ScopeTitle", "ScopeText", NULL);
    control(c, d, "Scope", "RadioButtonGroup", 25, 60, 320, 36, VIS | EN, "RPSCOPE", NULL, "Back");
    const char *vals[2] = { "user", "machine" }, *labels[2] = { T(c, "ScopeUser"), T(c, "ScopeMachine") };
    for (int i = 0; i < 2; ++i) {
        rows_t *r = &c->radio;
        s_(c, r, "RPSCOPE"); i_(c, r, i + 1); s_(c, r, vals[i]); i_(c, r, 0);
        i_(c, r, i * 18); i_(c, r, 320); i_(c, r, 16); s_(c, r, labels[i]); n_(c, r);
    }
    buttons(c, d, back, next, next_text, NULL, "Scope");
    event(c, d, "Next", "[ALLUSERS]", "2", "RPSCOPE = \"user\"", 0);
    event(c, d, "Next", "[MSIINSTALLPERUSER]", "1", "RPSCOPE = \"user\"", 0);
    event(c, d, "Next", "[ALLUSERS]", "1", "RPSCOPE = \"machine\"", 0);
    event(c, d, "Next", "[MSIINSTALLPERUSER]", "{}", "RPSCOPE = \"machine\"", 0);
    const char *pu = dir_id ? scope_path(c, dir_id, false) : NULL, *pm = dir_id ? scope_path(c, dir_id, true) : NULL;
    if (pu && pm) {
        event(c, d, "Next", keep(c, "[", keep(c, dir, "]")), pu, "RPSCOPE = \"user\"", 0);
        event(c, d, "Next", keep(c, "[", keep(c, dir, "]")), pm, "RPSCOPE = \"machine\"", 0);
        event(c, d, "Next", "SetTargetPath", dir, NULL, 0);
    }
    prop(c, "RPSCOPE", "user");
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

// The label of value j of a radio or combo control, in every language (labels-xx).
static const char *label(ctx_t *c, const rp_ir_dialog_control_t *x, size_t j) {
    rp_ir_ltext_t by[RP_UI_LANG_MAX];
    size_t n = 0;
    for (size_t k = 0; k < x->labels_by_lang_count && n < RP_UI_LANG_MAX; ++k) by[n++] = (rp_ir_ltext_t){ x->labels_by_lang[k].lang, x->labels_by_lang[k].labels[j] };
    return AT(c, x->labels[j], by, n, false);
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
    frame_text(c, d->id, AT(c, d->title ? d->title : "[ProductName]", d->title_by_lang, d->title_by_lang_count, true),
               AT(c, d->description, d->description_by_lang, d->description_by_lang_count, false));
    for (size_t i = 0; i < n; ++i) {
        const rp_ir_dialog_control_t *x = mine[i];
        const char *text = AT(c, x->text, x->text_by_lang, x->text_by_lang_count, false);
        const char *to = NULL;              // the next tab stop, or Back after the last
        if (x->type != RP_DC_TEXT) {
            to = "Back";
            for (size_t j = i + 1; j < n && strcmp(to, "Back") == 0; ++j) {
                if (mine[j]->type != RP_DC_TEXT) to = mine[j]->id;
            }
        }
        switch (x->type) {
        case RP_DC_TEXT:
            control(c, d->id, x->id, "Text", x->x, x->y, x->width, x->height, VIS | TRANSPARENT, NULL, text, NULL);
            break;
        case RP_DC_CHECKBOX:        // ticked: the property is "1"; clear: the property is removed
            control(c, d->id, x->id, "CheckBox", x->x, x->y, x->width, x->height, VIS | EN, x->property, text, to);
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
                i_(c, r, (int32_t)j * step); i_(c, r, x->width); i_(c, r, step < 14 ? step : 14); s_(c, r, label(c, x, j)); n_(c, r);
            }
            break;
        }
        case RP_DC_COMBO:           // a drop-down list (0x20000): one of the values, nothing typed. Without
                                    // Sorted (0x10000) the engine lists them alphabetically (observed).
            control(c, d->id, x->id, "ComboBox", x->x, x->y, x->width, x->height, VIS | EN | SUNKEN | 0x20000 | 0x10000,
                    x->property, NULL, to);
            for (size_t j = 0; j < x->value_count; ++j) {
                rows_t *r = &c->combo;
                s_(c, r, x->property); i_(c, r, (int32_t)j + 1); s_(c, r, x->values[j]); s_(c, r, label(c, x, j));
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

// ---- several languages (RFC-0012) ----------------------------------------------------------------

// The condition that language li is the chosen one. English is also chosen for an unknown value.
static const char *lang_cond(ctx_t *c, size_t li) {
    if (li) return keep(c, "RPLANGUAGE = \"", keep(c, lang_code(c, li), "\""));
    const char *s = "NOT (";
    for (size_t i = 1; i < c->nlang; ++i) s = keep(c, s, keep(c, i > 1 ? " OR RPLANGUAGE = \"" : "RPLANGUAGE = \"", keep(c, lang_code(c, i), "\"")));
    return keep(c, s, ")");
}

// The first page when the dialogs speak several languages: the heading and the line under it in
// every language, a radio button per language (the automatic choice already selected), then the
// welcome page or, when installed, the maintenance page.
static void language_dlg(ctx_t *c) {
    const char *d = "RpLanguageDlg";
    const char *title = "", *text = "";
    for (size_t i = 0; i < c->nlang; ++i) {
        title = keep(c, title, keep(c, i ? " / " : "", text_for(c, "LanguageTitle", i)));
        text = keep(c, text, keep(c, i ? " / " : "", text_for(c, "LanguageText", i)));
    }
    dialog(c, d, 370, 270, 3, "Lang", "Next", "Cancel");
    frame_text(c, d, keep(c, "{\\RpTitle_en}", title), text);
    int h = 14 * (int)c->nlang;
    control(c, d, "Lang", "RadioButtonGroup", 25, 60, 200, h, VIS | EN, "RPLANGUAGE", NULL, "Back");
    for (size_t i = 0; i < c->nlang; ++i) {
        rows_t *r = &c->radio;
        const char *name = c->ir->ui_langs[i].name ? c->ir->ui_langs[i].name : lang_code(c, i);
        s_(c, r, "RPLANGUAGE"); i_(c, r, (int32_t)i + 1); s_(c, r, lang_code(c, i)); i_(c, r, 0);
        i_(c, r, (int32_t)i * 14); i_(c, r, 200); i_(c, r, 14); s_(c, r, name); n_(c, r);
    }
    control(c, d, "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), "Next");
    control(c, d, "Next", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Next"), "Cancel");
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "Lang");
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    // Next: the texts and fonts of the chosen language first (language_rows), then the page.
    event(c, d, "Next", "NewDialog", "RpWelcomeDlg", "NOT Installed", 2);
    event(c, d, "Next", "NewDialog", "RpMaintenanceDlg", "Installed", 2);
    seq(c, d, "NOT Installed OR (NOT RESUME AND NOT Preselected)", 1225);
}

// Settles a text for one language at build time: the source's own values and [\x] escapes put in.
// Returns NULL when the text needs the installer's formatting (rp_ui_text_static).
static const char *settle(ctx_t *c, const char *src) {
    if (!rp_ui_text_static(src)) return NULL;
    const char *vals[3] = { c->ir->name, c->ir->manufacturer, c->ir->version };
    const char *out = "";
    const char *p = src;
    while (*p) {
        const char *b = strchr(p, '[');
        if (b == NULL) {
            out = keep(c, out, p);
            break;
        }
        char *head = rp_mem_alloc(c->alloc, (size_t)(b - p) + 1, 1);
        if (head == NULL) {
            c->nomem = true;
            return "";
        }
        memcpy(head, p, (size_t)(b - p));
        head[b - p] = 0;
        out = keep(c, out, head);
        rp_mem_free(c->alloc, head);
        const char *e = strchr(b, ']');
        size_t n = (size_t)(e - b - 1);
        if (n == 2 && b[1] == '\\') {
            char one[2] = { b[2], 0 };
            out = keep(c, out, one);
        } else {
            for (size_t i = 0; i < 3; ++i) {
                if (strlen(static_props[i]) == n && strncmp(b + 1, static_props[i], n) == 0) out = keep(c, out, vals[i] ? vals[i] : "");
            }
        }
        p = e + 1;
    }
    return out;
}

// Rows that put the chosen language's texts into the RpT_* properties (the oracle of RFC-0012: a
// property's value is shown as it is, so each text is formatted when it is put in):
//   - Property RpT_<key>_<code>: the text settled at build time (with its title style);
//     RpT_<key>: the English one, the default;
//   - InstallUISequence, before anything is shown: RPLANGUAGE from the command line, else the
//     first language whose LANGIDs hold UserLanguageID, then SystemLanguageID, else English; then
//     one type-51 action per text of that language (English needs them only for formatted texts);
//   - the language page's Next: the same for the language picked there.
static void language_rows(ctx_t *c) {
    int n = 0;
    char name[40];
    for (size_t li = 1; li < c->nlang; ++li) {
        const rp_ir_ui_lang_t *L = &c->ir->ui_langs[li];
        const char *u = "", *sy = "";
        for (size_t j = 0; j < L->langid_count; ++j) {
            char id[16];
            snprintf(id, sizeof id, "%u", (unsigned)L->langids[j]);
            u = keep(c, u, keep(c, j ? " OR UserLanguageID = " : "UserLanguageID = ", id));
            sy = keep(c, sy, keep(c, j ? " OR SystemLanguageID = " : "SystemLanguageID = ", id));
        }
        snprintf(name, sizeof name, "RpLangU_%s", L->code);
        ca(c, keep(c, name, NULL), "RPLANGUAGE", L->code);
        seq(c, keep(c, name, NULL), keep(c, "NOT RPLANGUAGE AND (", keep(c, u, ")")), 17);
        snprintf(name, sizeof name, "RpLangS_%s", L->code);
        ca(c, keep(c, name, NULL), "RPLANGUAGE", L->code);
        seq(c, keep(c, name, NULL), keep(c, "NOT RPLANGUAGE AND (", keep(c, sy, ")")), 18);
    }
    ca(c, "RpLangEn", "RPLANGUAGE", "en");
    seq(c, "RpLangEn", "NOT RPLANGUAGE", 19);
    for (size_t li = 0; li < c->nlang; ++li) {
        const char *code = lang_code(c, li), *cnd = lang_cond(c, li);
        const char *font = keep(c, "RpNormal_", code);
        if (li) {
            snprintf(name, sizeof name, "RpL_%s_font", code);
            ca(c, keep(c, name, NULL), "DefaultUIFont", font);
            seq(c, keep(c, name, NULL), cnd, 21);
        }
        if (c->page) event(c, "RpLanguageDlg", "Next", "[DefaultUIFont]", font, cnd, 1);
        for (size_t k = 0; k < c->nreg; ++k) {
            const regtext_t *r = &c->reg[k];
            const char *src = r->src[li] ? r->src[li] : "";
            const char *pre = r->title ? keep(c, "{\\RpTitle_", keep(c, code, "}")) : "";
            const char *v = settle(c, src);
            const char *target = keep(c, "RpT_", r->key);
            const char *arg;
            if (v) {
                const char *pname = keep(c, target, keep(c, "_", code));
                const char *val = keep(c, pre, v);
                if (val[0]) prop(c, pname, val);        // an empty text: the property stays unset
                if (li == 0 && val[0]) prop(c, target, val);
                arg = keep(c, "[", keep(c, pname, "]"));
            } else {
                arg = keep(c, pre, src);
            }
            if (c->page) event(c, "RpLanguageDlg", "Next", keep(c, "[", keep(c, target, "]")), arg, cnd, 1);
            if (li || v == NULL) {
                snprintf(name, sizeof name, "RpL_%s_%d", code, ++n);
                ca(c, keep(c, name, NULL), target, arg);
                seq(c, keep(c, name, NULL), cnd, 21);
            }
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
    rp_msi_wtable_t *tables = rp_mem_alloc(alloc, 11, sizeof *tables);
    if (c == NULL || ui == NULL || tables == NULL) {
        rp_mem_free(alloc, c);
        rp_mem_free(alloc, ui);
        rp_mem_free(alloc, tables);
        return PROVEN_ERR_NOMEM;
    }
    memset(c, 0, sizeof *c);
    c->alloc = alloc;
    c->ir = ir;
    c->nlang = ir->ui_lang_count ? ir->ui_lang_count : 1;
    c->multi = c->nlang > 1;
    c->page = c->multi && ir->ui >= RP_UI_MINIMAL;
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
    rows_init(&c->listbox, "ListBox", listbox_cols, 4);

    // Fonts: each language's face (P1a: Hangul shows in the Korean face), Segoe UI for English.
    // With several languages the styles carry the language's code, and the chosen language picks
    // DefaultUIFont and the title style (RFC-0012).
    for (size_t i = 0; i < c->nlang; ++i) {
        const char *face = ir->ui_lang_count && ir->ui_langs[i].font ? ir->ui_langs[i].font : "Segoe UI";
        const char *sfx = c->multi ? keep(c, "_", lang_code(c, i)) : "";
        s_(c, &c->style, keep(c, "RpNormal", sfx)); s_(c, &c->style, face); i_(c, &c->style, 9); n_(c, &c->style); n_(c, &c->style);
        s_(c, &c->style, keep(c, "RpTitle", sfx)); s_(c, &c->style, face); i_(c, &c->style, 11); n_(c, &c->style); i_(c, &c->style, 1);
    }
    prop(c, "DefaultUIFont", c->multi ? "RpNormal_en" : "RpNormal");
    // The engine's own texts (sizes, feature tree menus) have one table: English.
    for (size_t i = 0; i < sizeof uitexts / sizeof uitexts[0]; ++i) {
        s_(c, &c->uitext, uitexts[i].id); s_(c, &c->uitext, uitexts[i].en);
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
    // The license in each language: its own (license-xx), else the common one, else English's.
    const char *rtf[RP_UI_LANG_MAX] = { 0 };
    bool any_license = false;
    for (size_t i = 0; i < c->nlang; ++i) {
        const uint8_t *b = in->license_rtf_by_lang[i];
        size_t n = in->license_len_by_lang[i];
        if (b == NULL) b = in->license_rtf, n = in->license_len;
        if (b == NULL) b = in->license_rtf_by_lang[0], n = in->license_len_by_lang[0];
        if (b == NULL) continue;
        any_license = true;
        char *t = rp_mem_alloc(alloc, n + 1, 1);    // the RTF text as a string cell (7-bit ASCII: \uN? escapes)
        if (t == NULL) {
            c->nomem = true;
            continue;
        }
        memcpy(t, b, n);
        t[n] = 0;
        rtf[i] = keep(c, t, NULL);
        rp_mem_free(alloc, t);
    }
    for (size_t i = 0; any_license && i < c->nlang; ++i) {
        if (rtf[i] == NULL) rtf[i] = "";
    }
    const char *dir = in->install_dir ? in->install_dir : "TARGETDIR";
    if (c->page) language_dlg(c);
    if (ir->ui >= RP_UI_MINIMAL) {
        // The pages in order: the set's own, each followed by the author's pages placed after it.
        size_t cap = 6 + ir->dialog_count, n = 0;
        page_t *pages = rp_mem_alloc(alloc, cap, sizeof *pages);
        if (pages == NULL) c->nomem = true;
        const char *base[6];
        size_t nb = 0;
        base[nb++] = "RpWelcomeDlg";
        if (any_license) base[nb++] = "RpLicenseDlg";
        if (ir->scope == 2) base[nb++] = "RpScopeDlg";          // RFC-0013 A6
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
            if (pages[i].custom) custom_dlg(c, pages[i].custom, back ? back : (c->page ? "RpLanguageDlg" : NULL), next, next_text);
            else if (strcmp(p, "RpWelcomeDlg") == 0) welcome_dlg(c, next, next_text);
            else if (strcmp(p, "RpLicenseDlg") == 0) license_dlg(c, rtf, back, next, next_text);
            else if (strcmp(p, "RpInstallDirDlg") == 0) installdir_dlg(c, back, next, dir);
            else if (strcmp(p, "RpScopeDlg") == 0) scope_dlg(c, back, next, next_text, dir, ir->ui_install_dir ? ir->ui_install_dir : "INSTALLDIR");
            else if (strcmp(p, "RpCustomizeDlg") == 0) customize_dlg(c, back, next);
            else ready_dlg(c, back);
        }
        rp_mem_free(alloc, pages);
        maintenance_dlg(c, ir->ui == RP_UI_FEATURES);
        if (ir->ui >= RP_UI_INSTALLDIR) prop(c, "_RpBrowseProperty", dir);
    }
    if (c->multi) language_rows(c);

    rows_t *all[] = { &c->dialog, &c->control, &c->event, &c->condition, &c->mapping, &c->style, &c->uitext, &c->binary, &c->radio, &c->combo, &c->listbox };
    size_t nt = 0;
    for (size_t i = 0; i < 11; ++i) {
        rows_t *r = all[i];
        // FilesInUse fills its ListBox from the ListBox table, which must exist even when empty
        // (error 2205 otherwise, and the dialog is skipped: observed). ControlCondition is kept
        // when empty too: ICE17 reads it for every dialog and stops with 2228 without it.
        if (r->filled == 0 && r != &c->listbox && r != &c->condition) continue;
        tables[nt++] = (rp_msi_wtable_t){ r->name, r->cols, r->ncols, r->cells, r->filled / r->ncols };
    }
    ui->tables = tables;
    ui->table_count = nt;
    ui->props = c->props;
    ui->prop_count = c->nprops;
    ui->seqs = c->seqs;
    ui->seq_count = c->nseqs;
    ui->cas = c->cas;
    ui->ca_count = c->ncas;
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
        rows_t *all[] = { &c->dialog, &c->control, &c->event, &c->condition, &c->mapping, &c->style, &c->uitext, &c->binary, &c->radio, &c->combo, &c->listbox };
        for (size_t i = 0; i < 11; ++i) rp_mem_free(alloc, all[i]->cells);
        for (size_t i = 0; i < c->nstr; ++i) rp_mem_free(alloc, c->strings[i]);
        rp_mem_free(alloc, c->strings);
        rp_mem_free(alloc, c->props);
        rp_mem_free(alloc, c->seqs);
        rp_mem_free(alloc, c->cas);
        rp_mem_free(alloc, c->reg);
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
