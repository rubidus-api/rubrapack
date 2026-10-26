// src/msi/ui.c - the built-in dialog sets (include/rubrapack/ui.h, RFC-0005).
//
// Every dialog is 370 x 270 dialog units: a white banner (44 high) with a bold title and a line of
// description, the body, a line at 234 and the buttons Back / Next / Cancel at y 243. Nothing is
// drawn from bitmaps except the banner fill (a 1 x 1 white BMP, stretched) unless the source gives
// its own banner. The wording below is rubrapack's own, in Korean and English.

#include "rubrapack/ui.h"
#include "rubrapack/lang.h"
#include "rubrapack/mem.h"

#include <stdio.h>
#include <string.h>

// ---- texts ------------------------------------------------------------------------------------

// A text's ID and its English. Every other language is a language pack (include/rubrapack/lang.h):
// its entry with msgctxt "installer" and this ID; Korean is resources/lang/ko.po.
typedef struct {
    const char *id, *en;
} text_t;

static const text_t texts[] = {
    { "Back", "< &Back" },
    { "Next", "&Next >" },
    { "Cancel", "Cancel" },
    { "Install", "&Install" },
    { "Finish", "&Finish" },
    { "OK", "OK" },
    { "Yes", "&Yes" },
    { "No", "&No" },
    { "Retry", "&Retry" },
    { "Ignore", "&Ignore" },
    { "Abort", "&Abort" },
    { "Exit", "E&xit" },
    { "Browse", "B&rowse..." },
    { "WelcomeTitle", "Welcome to [ProductName] Setup" },
    { "WelcomeText", "This will install [ProductName] on your computer. Click Next to continue." },
    { "LicenseTitle", "License agreement" },
    { "LicenseText", "Please read the following license agreement." },
    { "LicenseAccept", "I &accept the terms of the license agreement" },
    { "DirTitle", "Installation folder" },
    { "DirText", "Choose the folder to install [ProductName] in." },
    { "DirLabel", "Install &to:" },
    { "BrowseTitle", "Change the folder" },
    { "BrowseText", "Browse to the folder to install in." },
    { "BrowseLookIn", "&Look in:" },
    { "BrowseFolder", "&Folder name:" },
    { "BrowseUp", "Up" },
    { "BrowseNew", "New folder" },
    { "CustomizeTitle", "Choose features" },
    { "CustomizeText", "Choose the features to install." },
    { "Reset", "Re&set" },
    { "DiskCost", "&Disk usage" },
    { "DiskCostTitle", "Disk space" },
    { "DiskCostText", "Space needed for the chosen features, and space available." },
    { "ReadyTitle", "Ready to install" },
    { "ReadyText", "Click Install to install [ProductName]." },
    { "ProgressTitle", "Installing [ProductName]" },
    { "ProgressText", "Please wait." },
    { "ProgressStatus", "Status:" },
    { "ExitTitle", "Setup complete" },
    { "ExitText", "[ProductName] has been installed." },
    // The same pages during a removal (RP_REMOVAL_COND). A language other than English and
    // Korean that gives no Removal* text gets its own install text instead (rp_ui_text_for).
    { "RemovalProgressTitle", "Removing [ProductName]" },
    { "RemovalExitTitle", "Removal complete" },
    { "RemovalExitText", "[ProductName] has been removed." },
    { "RemovalUserExitTitle", "Removal cancelled" },
    { "RemovalUserExitText", "The removal was cancelled. Nothing on your computer was changed." },
    { "RemovalFatalTitle", "Removal failed" },
    { "RemovalFatalText", "An error ended the removal, which was rolled back. The product is still installed." },
    { "UserExitTitle", "Setup cancelled" },
    { "UserExitText", "Setup was cancelled. Nothing on your computer was changed." },
    { "FatalTitle", "Setup failed" },
    { "FatalText", "An error ended the installation, which was rolled back. Nothing on your computer was changed." },
    { "CancelText", "Cancel the installation of [ProductName]?" },
    { "FilesInUseTitle", "Files in use" },
    // Measured (DECISIONS 2026-10-02 "Files in use"): with Restart Manager off (every package) the
    // engine moves a held file aside and puts the new one in place at once; the old copy goes at
    // the next restart, which with reboot = "suppress" (the default) nobody is asked for.
    { "FilesInUseText", "These programs use files that are being replaced. Click Continue to replace them now: programs already open keep the old files until they are reopened, programs opened afterwards get the new ones. No restart is needed." },
    { "FilesInUseTextRestart", "These programs use files that are being replaced. Click Continue to replace them now: programs already open keep the old files until they are reopened, programs opened afterwards get the new ones. Windows may ask to restart at the end." },
    { "Continue", "&Continue" },
    { "OutOfDiskTitle", "Not enough disk space" },
    { "OutOfDiskText", "There is not enough space for this installation. Free some space or choose another location." },
    { "MaintTitle", "Maintain [ProductName]" },
    { "MaintText", "Choose what to do." },
    { "Repair", "Re&pair" },
    { "RepairText", "Restore missing or damaged files and settings." },
    { "Remove", "&Remove" },
    { "RemoveText", "Remove [ProductName] from this computer." },
    // The second question before a removal chosen on the maintenance page (RpRemoveDlg), and the
    // same in one sentence for the window of a package without dialogs (RemoveAsk). The *All texts
    // are for a package that wipes what the program made ([remove] recursive, [registry] on =
    // "uninstall"). A language that does not give them gets English.
    { "RemoveConfirmTitle", "Remove [ProductName]" },
    { "RemoveConfirmText", "Click Remove to remove [ProductName] from this computer. Click Back or Cancel to keep it." },
    { "RemoveConfirmTextAll", "Click Remove to remove [ProductName] from this computer. Its settings and data are deleted with it and cannot be brought back. Click Back or Cancel to keep it." },
    { "RemoveAsk", "Remove [ProductName] from this computer?" },
    { "RemoveAskAll", "Remove [ProductName] from this computer? Its settings and data are deleted with it and cannot be brought back." },
    // The preflight's messages (helper DLL, RpPreflight): [1] how many or what, [2] the list or the
    // removal command, [3] the product. A language that does not give them gets English.
    { "PreflightAsk", "[1] program(s) are using files of [3]:\r\n[2]\r\nClose them all?\r\n\r\nYes: close them all and go on (unsaved work may be lost).\r\nNo: go on without closing (programs already open keep the old files until they are reopened).\r\nCancel: stop." },
    { "PreflightSilent", "[3]: stopped, because [1] program(s) are using its files:\r\n[2]Close them and try again, or give RPCLOSE=yes (close them all and go on) or RPCLOSE=no (go on without closing)." },
    { "PreflightFolder", "[3] cannot be installed safely: [1] is not a folder, or the installer may not delete files in it. Remove [3] first ([2]), then install again." },
    { "PreflightCache", "[3] cannot be upgraded safely: the installed version [1] has lost its cached package, so it cannot be removed here. Remove that version with its original package first ([2]), then install again." },
    // Before a removal (DECISIONS 2026-10-14): what is in the product's folders but was not installed
    // with it - [1] how many, [2] the list, [3] the product. A language that does not give them gets English.
    { "PreflightExtra", "[1] item(s) in the folders of [3] were not installed with it:\r\n[2]\r\nDelete them too?\r\n\r\nYes: delete them with [3] (they cannot be brought back).\r\nNo: remove [3] and keep them.\r\nCancel: stop." },
    { "PreflightExtraSilent", "[3]: stopped, because [1] item(s) in its folders were not installed with it:\r\n[2]Give RPEXTRA=delete (delete them too) or RPEXTRA=keep (keep them), then remove it again." },
    // Before a removal (RFC-0030): [1] how many other users of the computer may have settings or
    // data of [3], [2] their names.
    { "PreflightUsers", "[1] other user(s) of this computer may have settings or data of [3]:\r\n[2]\r\nDelete theirs too?\r\n\r\nYes: delete them for every user (they cannot be brought back).\r\nNo: delete only yours.\r\nCancel: stop." },
    // Before a first install ([other-copy.*]): [1] this product, [2] the copy found (an MSIX's full
    // package name, or an MSI's product name).
    { "PreflightOtherStore", "[1] is already installed on this computer from the Microsoft Store or as an app package ([2]). Two copies of one program both open its files and both show its menus.\r\n\r\nInstall this one too?" },
    { "PreflightOtherStoreRefuse", "[1] is already installed on this computer from the Microsoft Store or as an app package ([2]). Remove that copy first (Settings > Apps), then install this one." },
    { "PreflightOtherMsi", "Another installation of [1] is already on this computer ([2]). Two copies of one program both open its files and both show its menus.\r\n\r\nInstall this one too?" },
    { "PreflightOtherMsiRefuse", "Another installation of [1] is already on this computer ([2]). Remove it first (Settings > Apps), then install this one." },
    // The double-failure report (RFC-0003 6): its first paragraph, one per kind of run. [1] the
    // product, [2] the installation's log. The lines after it name what differs.
    { "ReportInstall", "[1] could not be installed, and undoing that did not remove everything. What is left is listed below. Run the installer again, then remove [1] if you do not want it.\r\nLog: [2]" },
    { "ReportUpgrade", "[1] could not be upgraded, and undoing that did not restore the version you had. What differs is listed below. Repair it with that version's installer, or install the new version again.\r\nLog: [2]" },
    { "ReportRemove", "[1] could not be removed, and undoing that did not restore it. What differs is listed below. Repair it with its installer, then remove it again.\r\nLog: [2]" },
    { "ReportRepair", "[1] could not be repaired, and undoing that did not restore it. What differs is listed below. Run the installer again to repair it.\r\nLog: [2]" },
    { "DirGuardText", "Setup stopped: the folder [1] already exists and is not owned by administrators, or it leads somewhere else through a link. Choose another folder, or have an administrator remove it first." },
    { "LaunchText", "&Launch [ProductName]" },
    { "SaveLog", "&Save log..." },
    // The progress page's details box ([ui] details): its check box shows the log's own lines.
    // Messages of a package whatever its dialogs: in the package's language ([package] language).
    { "PerUserOnly", "[ProductName] installs for the current user only (MSIINSTALLPERUSER=1)." },
    { "NewerInstalled", "The same or a newer version of [ProductName] is already installed." },
    { "OlderRefused", "The installed older version of [ProductName] cannot be upgraded by this package. Remove it first, then run this installation again:" },
    // The install folder page of a package that wipes that folder at removal ([remove] recursive):
    // a folder that already holds something is asked about (RpDirWarnDlg).
    { "DirNotEmptyText", "This folder already has files or folders in it. When [ProductName] is removed, everything in the folder is deleted - what is there now too. An empty or a new folder is the safe choice." },
    { "DirChoose", "&Choose another" },
    { "DirContinue", "&Use this folder" },
    { "DetailsFull", "Show the full &log" },
    { "SaveLogFail", "The setup log could not be saved to [1]." },
    { "Change", "&Change" },
    { "ChangeText", "Choose which features are installed." },
    { "ScopeTitle", "Installation scope" },
    { "ScopeText", "Choose who can use [ProductName]." },
    { "ScopeUser", "Just &me" },
    { "ScopeMachine", "&Everyone on this computer - needs administrator rights" },
    { "LanguageTitle", "Language" },
    { "LanguageText", "Choose the language for setup." },
    // RFC-0028: the regions of a grouped language page (a pack's X-Rubrapack-Group; 9: none named).
    { "LangGroup1", "East Asia" },
    { "LangGroup2", "Southeast Asia" },
    { "LangGroup3", "South Asia" },
    { "LangGroup4", "Central Asia and the Caucasus" },
    { "LangGroup5", "Middle East" },
    { "LangGroup6", "Western and Southern Europe" },
    { "LangGroup7", "Northern Europe and the Baltics" },
    { "LangGroup8", "Central and Eastern Europe" },
    { "LangGroup9", "Other" },
};

bool rp_ui_text_known(const char *id) {
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        if (strcmp(texts[i].id, id) == 0) return true;
    }
    return false;
}

// The text `id` without the source's own: the language's pack, else English.
const char *rp_ui_text_builtin(const char *id, const char *code) {
    if (strcmp(code, "en") != 0) {
        const char *t = rp_langpack_text(rp_langpack_get(code, NULL), "installer", id);
        if (t) return t;
    }
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
        if (strcmp(texts[i].id, id) == 0) return texts[i].en;
    }
    return id;
}

bool rp_ui_text_in_pack(const char *id, const char *code) {
    return strcmp(code, "en") == 0 || rp_langpack_text(rp_langpack_get(code, NULL), "installer", id) != NULL;
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
// The progress page's status line: what each of Windows Installer's actions is doing (the
// ActionText table). Without the table Windows Installer shows its own words, in the language of
// Windows - English under a Korean dialog, on an English Windows. The template is the line the log
// gets for each item, in English as Windows Installer's own.
typedef struct {
    const char *action, *en, *tmpl;         // a pack's entry is "Action.<action>"
} action_text_t;

static const action_text_t action_texts[] = {
    { "AppSearch", "Searching for installed applications", "Property: [1], Signature: [2]" },
    { "CostFinalize", "Computing space requirements", NULL },
    { "CostInitialize", "Computing space requirements", NULL },
    { "CreateFolders", "Creating folders", "Folder: [1]" },
    { "CreateShortcuts", "Creating shortcuts", "Shortcut: [1]" },
    { "DeleteServices", "Deleting services", "Service: [1]" },
    { "DuplicateFiles", "Creating duplicate files", "File: [1], Directory: [9], Size: [6]" },
    { "FileCost", "Computing space requirements", NULL },
    { "FindRelatedProducts", "Searching for related applications", "Found application: [1]" },
    { "GenerateScript", "Generating script operations for action:", "[1]" },
    { "InstallFiles", "Copying new files", "File: [1], Directory: [9], Size: [6]" },
    { "InstallServices", "Installing new services", "Service: [2]" },
    { "InstallValidate", "Validating install", NULL },
    { "LaunchConditions", "Evaluating launch conditions", NULL },
    { "MigrateFeatureStates", "Migrating feature states from related applications", "Application: [1]" },
    { "MoveFiles", "Moving files", "File: [1], Directory: [9], Size: [6]" },
    { "PatchFiles", "Patching files", "File: [1], Directory: [2], Size: [3]" },
    { "ProcessComponents", "Updating component registration", NULL },
    { "PublishFeatures", "Publishing product features", "Feature: [1]" },
    { "PublishProduct", "Publishing product information", NULL },
    { "RegisterClassInfo", "Registering class servers", "Class ID: [1]" },
    { "RegisterExtensionInfo", "Registering extension servers", "Extension: [1]" },
    { "RegisterFonts", "Registering fonts", "Font: [1]" },
    { "RegisterMIMEInfo", "Registering MIME info", "MIME Content Type: [1], Extension: [2]" },
    { "RegisterProduct", "Registering product", "[1]" },
    { "RegisterProgIdInfo", "Registering program identifiers", "ProgID: [1]" },
    { "RegisterUser", "Registering user", "[1]" },
    { "RemoveEnvironmentStrings", "Updating environment strings", "Name: [1], Value: [2], Action [3]" },
    { "RemoveExistingProducts", "Removing the older version", "Application: [1], Command line: [2]" },
    { "RemoveFiles", "Removing files", "File: [1], Directory: [9]" },
    { "RemoveFolders", "Removing folders", "Folder: [1]" },
    { "RemoveIniValues", "Removing INI file entries", "File: [1], Section: [2], Key: [3], Value: [4]" },
    { "RemoveRegistryValues", "Removing system registry values", "Key: [1], Name: [2]" },
    { "RemoveShortcuts", "Removing shortcuts", "Shortcut: [1]" },
    { "Rollback", "Rolling back action:", "[1]" },
    { "RollbackCleanup", "Removing backup files", "File: [1]" },
    { "StartServices", "Starting services", "Service: [1]" },
    { "StopServices", "Stopping services", "Service: [1]" },
    { "UnpublishFeatures", "Unpublishing product features", "Feature: [1]" },
    { "UnregisterClassInfo", "Unregistering class servers", "Class ID: [1]" },
    { "UnregisterExtensionInfo", "Unregistering extension servers", "Extension: [1]" },
    { "UnregisterFonts", "Unregistering fonts", "Font: [1]" },
    { "UnregisterMIMEInfo", "Unregistering MIME info", "MIME Content Type: [1], Extension: [2]" },
    { "UnregisterProgIdInfo", "Unregistering program identifiers", "ProgID: [1]" },
    { "WriteEnvironmentStrings", "Updating environment strings", "Name: [1], Value: [2], Action [3]" },
    { "WriteIniValues", "Writing INI file values", "File: [1], Section: [2], Key: [3], Value: [4]" },
    { "WriteRegistryValues", "Writing system registry values", "Key: [1], Name: [2], Value: [3]" },
};

static const text_t uitexts[] = {
    { "AbsentPath", "" },
    { "bytes", "bytes" },
    { "GB", "GB" },
    { "KB", "KB" },
    { "MB", "MB" },
    { "MenuAbsent", "Entire feature will be unavailable" },
    { "MenuAllLocal", "Entire feature will be installed on local hard drive" },
    { "MenuLocal", "Will be installed on local hard drive" },
    { "MenuAdvertise", "Will be installed when required" },
    { "NewFolder", "Folder|New Folder" },
    { "SelAbsentAbsent", "This feature will remain uninstalled." },
    { "SelAbsentLocal", "This feature will be installed on the local hard drive." },
    { "SelLocalAbsent", "This feature will be removed." },
    { "SelLocalLocal", "This feature will remain on your local hard drive." },
    { "SelChildCostNeg", "This feature frees up [1] on your hard drive." },
    { "SelChildCostPos", "This feature requires [1] on your hard drive." },
    { "SelCostPending", "Compiling cost for this feature..." },
    { "SelParentCostNegNeg", "This feature frees up [1]. It has [2] of [3] subfeatures selected, which free up [4]." },
    { "SelParentCostNegPos", "This feature frees up [1]. It has [2] of [3] subfeatures selected, which require [4]." },
    { "SelParentCostPosNeg", "This feature requires [1]. It has [2] of [3] subfeatures selected, which free up [4]." },
    { "SelParentCostPosPos", "This feature requires [1]. It has [2] of [3] subfeatures selected, which require [4]." },
    { "TimeRemaining", "Time remaining: {[1] minutes }{[2] seconds}" },
    { "VolumeCostAvailable", "Available" },
    { "VolumeCostDifference", "Difference" },
    { "VolumeCostRequired", "Required" },
    { "VolumeCostSize", "Disk size" },
    { "VolumeCostVolume", "Volume" },
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
    bool               table;           // ... past RP_UI_LANG_ACTIONS: the texts are RpText rows (RFC-0028)
    bool               grouped;         // ... past RP_UI_LANG_LIST: the language page groups them by region
    bool               page;            // ... and the language page leads the flow
    bool               act;             // the status line's texts are the package's (ActionText)
    bool               details;         // the progress page has its details box (the helper DLL is there)
    bool               details_dll;     // the helper DLL is in the package (RP_DirCheck can be called)
    const char        *next_when;       // buttons(): Next moves on only when this holds (its events come later, at 5)
    bool               maint_back;      // buttons(): Back also leads to the maintenance page
    size_t             nlang;
    bool               rtl_any;         // a language reads right to left: every dialog has a mirrored twin
    bool               rtl;             // ... and the twins are being made now
    int                dw;              // the width of the dialog being made (to mirror its controls)
    const char        *dfirst;          // ... and its Control_First
    struct { const char *dlg, *ctl, *action, *expr; } *mcond;  // the exit pages' conditions, settled at the end (merge_rows)
    size_t             nmcond, capmcond;
    struct { const char *dlg, *ctl; bool vis; } *mctl;         // ... and their controls
    struct { const char *dlg, *def; } mdef[8];                 // ... and their default buttons
    size_t             nmdef;
    size_t             nmctl, capmctl;
    int                lseq;            // the next InstallUISequence number of a language action
    const rp_ui_control_image_t *images;    // RFC-0031 B10: bitmap dialog-controls' pictures
    size_t             image_count;
    const rp_ui_control_image_t *bbimages;  // RFC-0031 B10: billboards' pictures
    size_t             bbimage_count;
    bool               nomem;
    char             **strings;
    size_t             nstr, capstr;
    rows_t             dialog, control, event, condition, mapping, style, uitext, binary, radio, combo, listbox, acttext, rptext, billboard, bbcontrol;
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
static const rp_msi_wcolumn_t acttext_cols[] = { { "Action", KEY_S(72) }, { "Description", L_N(0) }, { "Template", L_N(0) } };
static const rp_msi_wcolumn_t binary_cols[] = { { "Name", KEY_S(72) }, { "Data", 0x0900u } };
static const rp_msi_wcolumn_t radio_cols[] = { { "Property", KEY_S(72) }, { "Order", KEY_I2 }, { "Value", S(64) },
    { "X", I2 }, { "Y", I2 }, { "Width", I2 }, { "Height", I2 }, { "Text", L_N(0) }, { "Help", L_N(50) } };
static const rp_msi_wcolumn_t listbox_cols[] = { { "Property", KEY_S(72) }, { "Order", KEY_I2 }, { "Value", S(64) },
    { "Text", L_N(64) } };
static const rp_msi_wcolumn_t combo_cols[] = { { "Property", KEY_S(72) }, { "Order", KEY_I2 }, { "Value", S(64) },
    { "Text", L_N(64) } };
// RFC-0028: a language's value of a property (Key) - Format 1: the installer formats it first.
static const rp_msi_wcolumn_t rptext_cols[] = { { "Lang", KEY_S(16) }, { "Key", KEY_S(72) }, { "Text", L_N(0) }, { "Format", I2 } };
// RFC-0031 B10: billboards shown on the progress page, and the controls (a Bitmap) drawn on each.
static const rp_msi_wcolumn_t billboard_cols[] = { { "Billboard", KEY_S(50) }, { "Feature_", S(38) }, { "Action", S_N(50) }, { "Ordering", I2_N } };
static const rp_msi_wcolumn_t bbcontrol_cols[] = { { "Billboard_", KEY_S(50) }, { "BBControl", KEY_S(50) }, { "Type", S(20) },
    { "X", I2 }, { "Y", I2 }, { "Width", I2 }, { "Height", I2 }, { "Attributes", I4_N }, { "Text", L_N(0) } };

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
    if (c->rtl) return;                     // the twins share the properties
    if (grow(c, (void **)&c->props, &c->capprops, c->nprops, sizeof *c->props)) c->props[c->nprops++] = (rp_ui_prop_t){ name, value };
}

// Language actions (type 51, one per text) get numbers of their own, which ICE82 wants: between
// LaunchConditions (100) and CostFinalize (1000), around CostInitialize (800) and FileCost (900) -
// 897 numbers, about 96 a language, so seven languages besides English fit; past 999 they share it.
static int lang_seq(ctx_t *c) {
    if (c->lseq < 101) c->lseq = 101;
    if (c->lseq == 800 || c->lseq == 900) ++c->lseq;
    return c->lseq < 999 ? c->lseq++ : 999;
}

static void seq(ctx_t *c, const char *action, const char *cond, int n) {
    if (grow(c, (void **)&c->seqs, &c->capseqs, c->nseqs, sizeof *c->seqs)) c->seqs[c->nseqs++] = (rp_ui_seq_t){ action, cond, n, false };
}

static void ca(ctx_t *c, const char *action, const char *source, const char *target) {
    if (grow(c, (void **)&c->cas, &c->capcas, c->ncas, sizeof *c->cas)) c->cas[c->ncas++] = (rp_ui_ca_t){ action, source, target, 51 };
}

// An immediate action of the helper DLL (Binary RpCa, which lower.c puts in).
static void ca_dll(ctx_t *c, const char *action, const char *entry) {
    if (grow(c, (void **)&c->cas, &c->capcas, c->ncas, sizeof *c->cas)) c->cas[c->ncas++] = (rp_ui_ca_t){ action, "RpCa", entry, 1 };
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
    // A language that gives no Removal* text shows its own install text instead.
    if (strncmp(id, "Removal", 7) == 0 && strcmp(code, "en") != 0 && rp_langpack_text(rp_langpack_get(code, NULL), "installer", id) == NULL)
        return rp_ui_text_for(ir, id + 7, li);
    return rp_ui_text_builtin(id, code);
}

const char *rp_ui_text_lang(const rp_ir_t *ir, const char *id, const char *code) {
    for (size_t i = 0; i < ir->ui_text_count; ++i) {
        const rp_ir_ui_text_t *x = &ir->ui_texts[i];
        if (strcmp(x->id, id) != 0) continue;
        for (size_t j = 0; j < x->by_lang_count; ++j) {
            if (strcmp(x->by_lang[j].lang, code) == 0) return x->by_lang[j].text;
        }
        if (x->text) return x->text;
    }
    return rp_ui_text_builtin(id, code);
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

// Right-to-left languages (X-Rubrapack-Direction: rtl). The Dialog and Control attributes are fixed
// per row while the language is chosen at run time, so a package with such a language has every
// dialog twice: as made, and mirrored as <name>_RTL - the controls at the other side, RTL reading
// order, right-aligned text, scroll bars on the left. The private property RpRtl picks the set:
// each dialog event and sequence row goes to both, with NOT RpRtl or RpRtl. FilesInUse, which the
// engine finds by its name, has no twin: it holds both sets, as the exit pages (merged).
enum { DLG_BIDI = 0x80 | 0x100 | 0x200, CTL_RTLRO = 0x20, CTL_RIGHT = 0x40, CTL_LEFTSCROLL = 0x80 };

// The exit pages are shown by their InstallUISequence numbers (-1, -2, -3), where the engine takes
// one row for each number (observed, x62): they hold both sets instead - the mirrored controls as
// <name>_R in the same dialog, each set shown by RpRtl (merge_rows). So does FilesInUse.
static bool merged(const ctx_t *c, const char *dlg) {
    return c->rtl_any && dlg && (strcmp(dlg, "RpExitDlg") == 0 || strcmp(dlg, "RpUserExitDlg") == 0 || strcmp(dlg, "RpFatalDlg") == 0 ||
                                 strcmp(dlg, "FilesInUse") == 0);
}

static const char *dn(ctx_t *c, const char *name) {
    return c->rtl && name && !merged(c, name) ? keep(c, name, "_RTL") : name;
}

// A control's name in a merged page's mirrored set.
static const char *mn(ctx_t *c, const char *dlg, const char *ctl) {
    return c->rtl && ctl && merged(c, dlg) ? keep(c, ctl, "_R") : ctl;
}

static const char *rtl_and(ctx_t *c, const char *cnd, bool rtl) {
    const char *r = rtl ? "RpRtl" : "NOT RpRtl";
    return cnd && strcmp(cnd, "1") != 0 ? keep(c, "(", keep(c, cnd, keep(c, ") AND ", r))) : r;
}

static void dialog(ctx_t *c, const char *name, int w, int h, int attrs, const char *first, const char *def,
                   const char *cancel) {
    rows_t *r = &c->dialog;
    c->dw = w;
    c->dfirst = first;
    if (c->rtl && merged(c, name)) return;  // the page has its row already
    if (merged(c, name) && def && c->nmdef < sizeof c->mdef / sizeof c->mdef[0]) c->mdef[c->nmdef++] = (typeof(c->mdef[0])){ name, def };
    if (c->rtl) name = dn(c, name), attrs |= DLG_BIDI;
    s_(c, r, name); i_(c, r, 50); i_(c, r, 50); i_(c, r, w); i_(c, r, h); i_(c, r, attrs);
    s_(c, r, "[ProductName]"); s_(c, r, first); s_(c, r, def); s_(c, r, cancel);
}

static void control(ctx_t *c, const char *dlg, const char *name, const char *type, int x, int y, int w, int h,
                    int attrs, const char *prop, const char *text, const char *next) {
    rows_t *r = &c->control;
    if (merged(c, dlg)) {
        // One tab cycle through both sets: the last control of each leads to the other's first.
        bool to_first = next && c->dfirst && strcmp(next, c->dfirst) == 0;
        if (!c->rtl && to_first) next = keep(c, next, "_R");
        else if (c->rtl && next && !to_first) next = keep(c, next, "_R");
        if (!c->rtl && grow(c, (void **)&c->mctl, &c->capmctl, c->nmctl, sizeof *c->mctl))
            c->mctl[c->nmctl++] = (typeof(*c->mctl)){ dlg, name, (attrs & VIS) != 0 };
        name = mn(c, dlg, name);
        if (c->rtl) attrs &= ~VIS;          // shown by its conditions
    }
    if (c->rtl) {
        dlg = dn(c, dlg);
        x = c->dw - x - w;
        if (strcmp(type, "PathEdit") == 0 || strcmp(type, "Edit") == 0 || strcmp(type, "DirectoryCombo") == 0) attrs |= CTL_RIGHT | CTL_LEFTSCROLL;
        else if (strcmp(type, "Line") != 0 && strcmp(type, "Bitmap") != 0 && strcmp(type, "Icon") != 0 && strcmp(type, "ProgressBar") != 0)
            attrs |= CTL_RTLRO | CTL_RIGHT | CTL_LEFTSCROLL;
    }
    s_(c, r, dlg); s_(c, r, name); s_(c, r, type); i_(c, r, x); i_(c, r, y); i_(c, r, w); i_(c, r, h); i_(c, r, attrs);
    s_(c, r, prop); s_(c, r, text); s_(c, r, next); n_(c, r);
}

static void event1(ctx_t *c, const char *dlg, const char *ctl, const char *ev, const char *arg, const char *cond, int order) {
    rows_t *r = &c->event;
    s_(c, r, dlg); s_(c, r, ctl); s_(c, r, ev); s_(c, r, arg); s_(c, r, cond ? cond : "1"); i_(c, r, order);
}

// An event; one that opens a dialog goes to both sets when there are twins.
static void event(ctx_t *c, const char *dlg, const char *ctl, const char *ev, const char *arg, const char *cond, int order) {
    ctl = mn(c, dlg, ctl);
    dlg = dn(c, dlg);
    bool opens = strcmp(ev, "NewDialog") == 0 || strcmp(ev, "SpawnDialog") == 0 || strcmp(ev, "SpawnWaitDialog") == 0;
    if (!c->rtl_any || !opens || strcmp(arg, "FilesInUse") == 0) {
        event1(c, dlg, ctl, ev, arg, cond, order);
        return;
    }
    event1(c, dlg, ctl, ev, arg, rtl_and(c, cond, false), order);
    event1(c, dlg, ctl, ev, keep(c, arg, "_RTL"), rtl_and(c, cond, true), order);
}

// A dialog's InstallUISequence row; with twins the set's own (the twin one number later, the exit
// dialogs at the same negative number).
static void dseq(ctx_t *c, const char *dlg, const char *cnd, int n) {
    if (merged(c, dlg)) {
        if (!c->rtl) seq(c, dlg, cnd, n);
        return;
    }
    if (!c->rtl_any) seq(c, dlg, cnd, n);
    else seq(c, dn(c, dlg), rtl_and(c, cnd, c->rtl), n < 0 || !c->rtl ? n : n + 1);
}

static void cond1(ctx_t *c, const char *dlg, const char *ctl, const char *action, const char *expr) {
    rows_t *r = &c->condition;
    s_(c, r, dlg); s_(c, r, ctl); s_(c, r, action); s_(c, r, expr);
}

static void cond(ctx_t *c, const char *dlg, const char *ctl, const char *action, const char *expr) {
    if (merged(c, dlg)) {                   // settled by merge_rows, for both sets
        if (!c->rtl && grow(c, (void **)&c->mcond, &c->capmcond, c->nmcond, sizeof *c->mcond))
            c->mcond[c->nmcond++] = (typeof(*c->mcond)){ dlg, ctl, action, expr };
        return;
    }
    cond1(c, dn(c, dlg), ctl, action, expr);
}

// The merged exit pages' conditions. The left-to-right control: its own conditions while not RpRtl,
// hidden with RpRtl. Its mirrored twin starts hidden: shown with RpRtl when the original would show.
static void merge_rows(ctx_t *c) {
    for (size_t k = 0; k < c->nmctl; ++k) {
        const char *dlg = c->mctl[k].dlg, *ctl = c->mctl[k].ctl, *twin = keep(c, ctl, "_R");
        const char *show_r = "RpRtl";
        bool hides = false;
        for (size_t j = 0; j < c->nmcond; ++j) {
            const char *a = c->mcond[j].action, *e = c->mcond[j].expr;
            if (strcmp(c->mcond[j].dlg, dlg) != 0 || strcmp(c->mcond[j].ctl, ctl) != 0) continue;
            if (strcmp(a, "Show") == 0) {
                cond1(c, dlg, ctl, a, keep(c, "(", keep(c, e, ") AND NOT RpRtl")));
                cond1(c, dlg, twin, a, keep(c, "(", keep(c, e, ") AND RpRtl")));
            } else if (strcmp(a, "Hide") == 0) {
                hides = true;
                cond1(c, dlg, ctl, a, keep(c, "(", keep(c, e, ") OR RpRtl")));
                cond1(c, dlg, twin, a, keep(c, "(", keep(c, e, ") OR NOT RpRtl")));
                show_r = keep(c, show_r, keep(c, " AND NOT (", keep(c, e, ")")));
            } else {
                cond1(c, dlg, ctl, a, e);
                cond1(c, dlg, twin, a, e);
            }
        }
        if (!c->mctl[k].vis) continue;
        if (!hides) cond1(c, dlg, ctl, "Hide", "RpRtl");
        cond1(c, dlg, twin, "Show", show_r);
    }
    // The Dialog row names the left-to-right default button, hidden with RpRtl: Enter did nothing on
    // a mirrored FilesInUse (x69). The mirrored one is made the default there.
    for (size_t k = 0; k < c->nmdef; ++k) cond1(c, c->mdef[k].dlg, keep(c, c->mdef[k].def, "_R"), "Default", "RpRtl");
}

static void mapping(ctx_t *c, const char *dlg, const char *ctl, const char *ev, const char *attr) {
    rows_t *r = &c->mapping;
    ctl = mn(c, dlg, ctl);
    dlg = dn(c, dlg);
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

// During a removal the title and, with `text`, the description of the Removal* texts show instead:
// the maintenance page's Remove sets RP_REMOVING (its Remove event leaves REMOVE unset - observed,
// x46); REMOVE = "ALL" is a removal asked for on the command line.
#define RP_REMOVAL_COND "RP_REMOVING = \"1\" OR REMOVE = \"ALL\""
static void frame_removal(ctx_t *c, const char *dlg, const char *title, const char *text) {
    char id[64];
    snprintf(id, sizeof id, "Removal%s", title);
    control(c, dlg, "RemovalTitle", "Text", 15, 7, 330, 15, TRANSPARENT | NOPREFIX, NULL, TT(c, keep(c, id, "")), NULL);
    cond(c, dlg, "RemovalTitle", "Show", RP_REMOVAL_COND);
    cond(c, dlg, "Title", "Hide", RP_REMOVAL_COND);
    if (text) {
        snprintf(id, sizeof id, "Removal%s", text);
        control(c, dlg, "RemovalDescription", "Text", 25, 22, 330, 20, TRANSPARENT | NOPREFIX, NULL, T(c, keep(c, id, "")), NULL);
        cond(c, dlg, "RemovalDescription", "Show", RP_REMOVAL_COND);
        cond(c, dlg, "Description", "Hide", RP_REMOVAL_COND);
    }
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
    control(c, dlg, "Back", "PushButton", 180, 243, 56, 17, back || c->maint_back ? VIS | EN : VIS, NULL, T(c, "Back"), "Next");
    control(c, dlg, "Next", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, next_text ? next_text : "Next"), "Cancel");
    control(c, dlg, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), first ? first : "Back");
    if (back && c->maint_back) {            // the feature tree, reached from the maintenance page too
        event(c, dlg, "Back", "NewDialog", back, "NOT Installed", 1);
        event(c, dlg, "Back", "NewDialog", "RpMaintenanceDlg", "Installed", 1);
    } else if (c->maint_back) {             // ... and from nowhere else (a set without the tree page)
        event(c, dlg, "Back", "NewDialog", "RpMaintenanceDlg", NULL, 1);
    } else if (back) {
        event(c, dlg, "Back", "NewDialog", back, NULL, 1);
    }
    if (next && next[0] && c->next_when) event(c, dlg, "Next", "NewDialog", next, c->next_when, 5);
    else if (next && next[0]) event(c, dlg, "Next", "NewDialog", next, next_cond_disable ? keep(c, "NOT (", keep(c, next_cond_disable, ")")) : NULL, 1);
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

// The engine shows it at InstallValidate when a program with a window holds a file to be replaced
// or removed (an input method or a shell extension: nearly every program). The engine's Ignore is
// the default, labelled Continue: the files are replaced at once (the text says how), so there is
// nothing to close; Retry and Exit stay.
static bool user_text(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; i < ir->ui_text_count; ++i) {
        if (strcmp(ir->ui_texts[i].id, id) == 0) return true;
    }
    return false;
}

static void files_in_use_dlg(ctx_t *c) {
    const char *d = "FilesInUse";           // the engine looks this dialog up by name
    const char *text = c->ir->reboot_suppress || user_text(c->ir, "FilesInUseText") ? "FilesInUseText" : "FilesInUseTextRestart";
    dialog(c, d, 370, 270, 3 | 32, "Ignore", "Ignore", "Exit");
    frame(c, d, "FilesInUseTitle", NULL, NULL);     // the text is too long for the banner (observed cut)
    control(c, d, "Text", "Text", 20, 52, 330, 40, VIS | NOPREFIX, NULL, T(c, text), NULL);
    control(c, d, "List", "ListBox", 20, 97, 330, 128, VIS | SUNKEN, "FileInUseProcess", NULL, NULL);
    // Retry wider, with a gap before Continue: Arabic's 75 px ran into Continue when mirrored (x69).
    control(c, d, "Retry", "PushButton", 166, 243, 66, 17, VIS | EN, NULL, T(c, "Retry"), "Exit");
    control(c, d, "Ignore", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Continue"), "Retry");
    control(c, d, "Exit", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Exit"), "Ignore");
    event(c, d, "Retry", "EndDialog", "Retry", NULL, 1);
    event(c, d, "Ignore", "EndDialog", "Ignore", NULL, 1);
    event(c, d, "Exit", "EndDialog", "Exit", NULL, 1);
}

// Progress (modeless) and the three exit dialogs.
static void progress_and_exits(ctx_t *c) {
    const char *d = "RpProgressDlg";
    dialog(c, d, 370, 270, 1, "Cancel", "Cancel", "Cancel");
    frame(c, d, "ProgressTitle", "ProgressText", NULL);
    frame_removal(c, d, "ProgressTitle", NULL);
    control(c, d, "StatusLabel", "Text", 25, 100, 50, 10, VIS | NOPREFIX, NULL, T(c, "ProgressStatus"), NULL);
    control(c, d, "ActionText", "Text", 75, 100, 270, 10, VIS | NOPREFIX, NULL, "", NULL);
    control(c, d, "ProgressBar", "ProgressBar", 25, 115, 320, 10, VIS | 0x10000, NULL, "", NULL);
    // RFC-0031 B10: billboards - a Billboard control in the upper body, and for each [billboard.*] a
    // Billboard row (shown while its feature's files install) with a Bitmap BBControl from a Binary.
    if (c->ir->billboard_count) {
        // The Billboard control goes on both the dialog and its mirrored twin; the Billboard,
        // BBControl and Binary rows are global, so they are written once (not again for the twin).
        control(c, d, "Billboard", "Billboard", 25, 52, 320, 44, VIS, NULL, NULL, NULL);
        for (size_t k = 0; !c->rtl && k < c->ir->billboard_count; ++k) {
            const rp_ir_billboard_t *bb = &c->ir->billboards[k];
            const char *bn = NULL;
            for (size_t g = 0; g < c->bbimage_count; ++g) {
                if (strcmp(c->bbimages[g].control_id, bb->id) != 0) continue;
                bn = keep(c, "RpBb", bb->id);
                s_(c, &c->binary, bn);
                rp_msi_cell_t *bc = cell(c, &c->binary);
                if (bc) { bc->bytes = (uint8_t *)c->bbimages[g].bmp; bc->len = c->bbimages[g].len; }
                break;
            }
            s_(c, &c->billboard, bb->id); s_(c, &c->billboard, bb->feature); s_(c, &c->billboard, "InstallFiles");
            i_(c, &c->billboard, bb->order ? bb->order : (int32_t)k + 1);
            s_(c, &c->bbcontrol, bb->id); s_(c, &c->bbcontrol, "Img"); s_(c, &c->bbcontrol, "Bitmap");
            i_(c, &c->bbcontrol, 0); i_(c, &c->bbcontrol, 0); i_(c, &c->bbcontrol, 320); i_(c, &c->bbcontrol, 44);
            i_(c, &c->bbcontrol, 1); s_(c, &c->bbcontrol, bn);
        }
    }
    control(c, d, "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), NULL);
    control(c, d, "Next", "PushButton", 236, 243, 56, 17, VIS, NULL, T(c, "Next"), NULL);
    // [ui] details: a scrolling box the helper DLL fills while the installation runs (RpDetails),
    // and the check box that turns it to the log's own lines. Empty RTF: the box starts blank.
    bool details = c->ir->ui_details && c->details;
    if (details) {
        control(c, d, "Details", "ScrollableText", 25, 132, 320, 82, VIS | EN | SUNKEN, NULL, "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Segoe UI;}}\\fs16 }", NULL);
        control(c, d, "FullLog", "CheckBox", 25, 218, 320, 12, VIS | EN, "RPDETAILSFULL", T(c, "DetailsFull"), "Cancel");
    }
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), details ? "FullLog" : NULL);
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    mapping(c, d, "ActionText", "ActionText", "Text");
    mapping(c, d, "ProgressBar", "SetProgress", "Progress");
    // A Billboard control is switched by the progress messages it subscribes to, as a progress bar
    // is (Microsoft Learn "ControlEvent Overview"; without it no billboard showed, x77-vm).
    if (c->ir->billboard_count) mapping(c, d, "Billboard", "SetProgress", "Progress");
    static const char *const exits[][3] = { { "RpExitDlg", "ExitTitle", "ExitText" },
                                            { "RpUserExitDlg", "UserExitTitle", "UserExitText" },
                                            { "RpFatalDlg", "FatalTitle", "FatalText" } };
    for (int i = 0; i < 3; ++i) {
        // RFC-0013 A5: the finished page may start the program (RP_Launch, made by the lowering),
        // after a first installation or an upgrade only.
        bool launch = i == 0 && c->ir->ui_launch_file;
        dialog(c, exits[i][0], 370, 270, 3, "Finish", "Finish", "Finish");
        frame(c, exits[i][0], exits[i][1], exits[i][2], NULL);
        frame_removal(c, exits[i][0], exits[i][1], exits[i][2]);
        // The log of this run (MsiLogging turns it on without /l): RP_SaveLog (lower.c) asks where
        // to save a copy. Hidden when Windows Installer keeps no log (MsiLogFileLocation empty).
        const char *after_finish = launch ? "Launch" : NULL;
        if (c->ir->ui_save_log) {
            control(c, exits[i][0], "SaveLog", "PushButton", 25, 243, 90, 17, VIS | EN, NULL, T(c, "SaveLog"), launch ? "Launch" : "Finish");
            cond(c, exits[i][0], "SaveLog", "Hide", "NOT MsiLogFileLocation");
            event(c, exits[i][0], "SaveLog", "DoAction", "RP_SaveLog", "MsiLogFileLocation", 1);
            after_finish = "SaveLog";
        }
        if (launch) {
            control(c, exits[i][0], "Launch", "CheckBox", 25, 80, 320, 17, VIS | EN, "RPLAUNCH", T(c, "LaunchText"), "Finish");
            cond(c, exits[i][0], "Launch", "Hide", "Installed");
            event(c, exits[i][0], "Finish", "DoAction", "RP_Launch", "RPLAUNCH = \"1\" AND NOT Installed", 1);
        }
        control(c, exits[i][0], "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), NULL);
        control(c, exits[i][0], "Finish", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Finish"), after_finish);
        control(c, exits[i][0], "Cancel", "PushButton", 304, 243, 56, 17, VIS, NULL, T(c, "Cancel"), NULL);
        event(c, exits[i][0], "Finish", "EndDialog", "Return", NULL, launch ? 2 : 1);
    }
    if (c->ir->ui_launch_file && c->ir->ui_launch_default) prop(c, "RPLAUNCH", "1");
    dseq(c, "RpExitDlg", NULL, -1);
    dseq(c, "RpUserExitDlg", NULL, -2);
    dseq(c, "RpFatalDlg", NULL, -3);
    dseq(c, "RpProgressDlg", NULL, 1280);
}

// A package wipes what the program made at removal ([remove] recursive, [registry] on = "uninstall").
bool rp_ui_wipes(const rp_ir_t *ir) {
    for (size_t i = 0; i < ir->remove_count; ++i) {
        if (ir->removes[i].recursive) return true;
    }
    for (size_t i = 0; i < ir->registry_count; ++i) {
        if (ir->registries[i].purge) return true;
    }
    return false;
}

// The package wipes the folder of dir `id` at removal (a [remove] recursive on that dir).
bool rp_ui_wipes_dir(const rp_ir_t *ir, const char *id) {
    for (size_t i = 0; i < ir->remove_count; ++i) {
        if (ir->removes[i].recursive && ir->removes[i].dir && strcmp(ir->removes[i].dir, id) == 0) return true;
    }
    return false;
}

// A feature the user may turn on or off: the maintenance page then has Change, in every set.
static bool choosable(const rp_ir_t *ir) {
    for (size_t i = 0; i < ir->feature_count; ++i) {
        const rp_ir_feature_t *f = &ir->features[i];
        if (!f->implicit && !f->hidden && !f->required) return true;
    }
    return false;
}

// RFC-0013 A4: with features to choose, Change (to the feature tree) comes first. Remove asks once
// more on a page of its own (RpRemoveDlg) before anything is removed.
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
    event(c, d, "Remove", "NewDialog", "RpRemoveDlg", NULL, 1);
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    if (!c->page) dseq(c, d, "Installed AND NOT RESUME AND NOT Preselected", 1240);

    const char *r = "RpRemoveDlg";          // Back is the default button: Enter does not remove
    dialog(c, r, 370, 270, 3, "Back", "Back", "Cancel");
    frame(c, r, "RemoveConfirmTitle", NULL, NULL);
    control(c, r, "Text", "Text", 25, 65, 320, 60, VIS | NOPREFIX, NULL, T(c, rp_ui_wipes(c->ir) ? "RemoveConfirmTextAll" : "RemoveConfirmText"), NULL);
    control(c, r, "Back", "PushButton", 180, 243, 56, 17, VIS | EN, NULL, T(c, "Back"), "Next");
    control(c, r, "Next", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Remove"), "Cancel");
    control(c, r, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "Back");
    event(c, r, "Back", "NewDialog", d, NULL, 1);
    event(c, r, "Next", "Remove", "ALL", NULL, 1);
    event(c, r, "Next", "[RP_REMOVING]", "1", NULL, 2);       // the Remove event leaves REMOVE unset (observed)
    event(c, r, "Next", "EndDialog", "Return", NULL, 3);
    event(c, r, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
}

static void welcome_dlg(ctx_t *c, const char *next, const char *next_text) {
    const char *d = "RpWelcomeDlg";
    dialog(c, d, 370, 270, 3, "Next", "Next", "Cancel");
    frame(c, d, "WelcomeTitle", NULL, NULL);
    control(c, d, "Body", "Text", 25, 60, 320, 100, VIS | NOPREFIX, NULL, T(c, "WelcomeText"), NULL);
    buttons(c, d, c->page ? "RpLanguageDlg" : NULL, next, next_text, NULL, NULL);
    if (!c->page) dseq(c, d, "NOT Installed", 1230);
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
    // A package that wipes this folder at removal asks before it takes a folder that holds
    // something: RP_DirCheck (helper DLL) looks, RpDirWarnDlg asks, and Next moves on only for an
    // empty or new folder or after "Use this folder". Not for an installed product (its own folder).
    bool ask = c->details_dll && rp_ui_wipes_dir(c->ir, c->ir->ui_install_dir ? c->ir->ui_install_dir : "INSTALLDIR");
    c->next_when = ask ? "RP_DIR_NOTEMPTY <> \"1\" OR Installed" : NULL;
    buttons(c, d, back, next, NULL, NULL, "Path");
    c->next_when = NULL;
    // Next first commits the path (SetTargetPath checks it), then moves on.
    event(c, d, "Next", "SetTargetPath", dir, NULL, 0);
    if (ask) {
        event(c, d, "Next", "DoAction", "RP_DirCheck", "NOT Installed", 2);
        event(c, d, "Next", "NewDialog", "RpDirWarnDlg", "RP_DIR_NOTEMPTY = \"1\" AND NOT Installed", 5);
        const char *w = "RpDirWarnDlg";         // Back ("Choose another") is the default button
        dialog(c, w, 370, 270, 3, "Back", "Back", "Cancel");
        frame(c, w, "DirTitle", NULL, NULL);
        control(c, w, "Text", "Text", 25, 65, 320, 70, VIS | NOPREFIX, NULL, T(c, "DirNotEmptyText"), NULL);
        control(c, w, "Back", "PushButton", 120, 243, 86, 17, VIS | EN, NULL, T(c, "DirChoose"), "Next");
        control(c, w, "Next", "PushButton", 210, 243, 86, 17, VIS | EN, NULL, T(c, "DirContinue"), "Cancel");
        control(c, w, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "Back");
        event(c, w, "Back", "NewDialog", d, NULL, 1);
        if (next && next[0]) event(c, w, "Next", "NewDialog", next, NULL, 1);
        else event(c, w, "Next", "EndDialog", "Return", NULL, 1);
        event(c, w, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    }

    const char *b = "RpBrowseDlg";
    dialog(c, b, 370, 270, 3, "PathEdit", "OK", "Cancel");
    frame(c, b, "BrowseTitle", "BrowseText", NULL);
    control(c, b, "LookIn", "Text", 20, 58, 50, 12, VIS, NULL, T(c, "BrowseLookIn"), NULL);
    // Up and New folder as wide as their longest translation (T158: 82 and 120 px at 1.38 a unit).
    control(c, b, "Combo", "DirectoryCombo", 72, 55, 125, 80, VIS | EN | INDIRECT | 0x70000 | 0x100000, "_RpBrowseProperty", NULL, "Up");
    control(c, b, "Up", "PushButton", 201, 55, 62, 17, VIS | EN, NULL, T(c, "BrowseUp"), "NewFolder");
    control(c, b, "NewFolder", "PushButton", 267, 55, 88, 17, VIS | EN, NULL, T(c, "BrowseNew"), "List");
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

static void customize_dlg(ctx_t *c, const char *back, const char *next, const char *next_text) {
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
    buttons(c, d, back, next, next_text, NULL, "Tree");
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
    for (int i = 0; i < 2 && !c->rtl; ++i) {      // the twin shares the buttons (they span the group)
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

// Text and bitmap controls are not tab stops (they take no focus), so the tab-order chain skips them.
static bool dc_tabstop(int type) { return type != RP_DC_TEXT && type != RP_DC_BITMAP; }

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
        if (dc_tabstop(mine[i]->type)) first = mine[i]->id;
    }
    dialog(c, d->id, 370, 270, 3, first ? first : "Next", "Next", "Cancel");
    frame_text(c, d->id, AT(c, d->title ? d->title : "[ProductName]", d->title_by_lang, d->title_by_lang_count, true),
               AT(c, d->description, d->description_by_lang, d->description_by_lang_count, false));
    for (size_t i = 0; i < n; ++i) {
        const rp_ir_dialog_control_t *x = mine[i];
        const char *text = AT(c, x->text, x->text_by_lang, x->text_by_lang_count, false);
        const char *to = NULL;              // the next tab stop, or Back after the last
        if (dc_tabstop(x->type)) {
            to = "Back";
            for (size_t j = i + 1; j < n && strcmp(to, "Back") == 0; ++j) {
                if (dc_tabstop(mine[j]->type)) to = mine[j]->id;
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
            for (size_t j = 0; j < x->value_count && !c->rtl; ++j) {
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
            for (size_t j = 0; j < x->value_count && !c->rtl; ++j) {
                rows_t *r = &c->combo;
                s_(c, r, x->property); i_(c, r, (int32_t)j + 1); s_(c, r, x->values[j]); s_(c, r, label(c, x, j));
            }
            break;
        case RP_DC_LISTBOX:         // a list box: one of the values is picked (no typing); shown in Order
            control(c, d->id, x->id, "ListBox", x->x, x->y, x->width, x->height, VIS | EN | SUNKEN, x->property, NULL, to);
            for (size_t j = 0; j < x->value_count && !c->rtl; ++j) {
                rows_t *r = &c->listbox;
                s_(c, r, x->property); i_(c, r, (int32_t)j + 1); s_(c, r, x->values[j]); s_(c, r, label(c, x, j));
            }
            break;
        case RP_DC_BITMAP: {        // a picture from a Binary stream (the .bmp read by the caller)
            const char *bn = NULL;
            for (size_t g = 0; g < c->image_count; ++g) {
                if (strcmp(c->images[g].control_id, x->id) != 0) continue;
                bn = keep(c, "RpBmp", x->id);
                if (!c->rtl) {          // the Binary row is global: add it once, not again for the RTL twin
                    rows_t *r = &c->binary;
                    s_(c, r, bn);
                    rp_msi_cell_t *bc = cell(c, r);
                    if (bc) { bc->bytes = (uint8_t *)c->images[g].bmp; bc->len = c->images[g].len; }
                }
                break;
            }
            control(c, d->id, x->id, "Bitmap", x->x, x->y, x->width, x->height, VIS, NULL, bn, NULL);   // stretched to the control rect, as the banner is
            break;
        }
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

// The region of language li on a grouped page: 1-8 from its pack, 9 for the rest.
static int lang_group(const ctx_t *c, size_t li) {
    int g = c->ir->ui_langs[li].group;
    return g >= 1 && g <= 8 ? g : 9;
}

// RFC-0028: past one column the regions are on the left (RpLangGroup, only those with a language)
// and each region's languages on the right, a radio group of its own (RpLangG_<n>: its own property,
// so that two groups never share buttons) on the same rectangle, shown while its region is chosen.
// RpLangApply sets both for the language it settles (RpText rows), so the page opens on it; Next
// copies the shown region's language into RPLANGUAGE.
static void grouped_page(ctx_t *c, const char *title, const char *text) {
    const char *d = "RpLanguageDlg";
    int count[10] = { 0 }, ng = 0, first = 0, last = 0;
    for (size_t li = 0; li < c->nlang; ++li) ++count[lang_group(c, li)];
    for (int g = 1; g <= 9; ++g) {
        if (count[g] == 0) continue;
        ++ng;
        if (first == 0) first = g;
        last = g;
    }
    char name[24], val[16], cnd[48];
    dialog(c, d, 370, 270, 3, "RpLangGroup", "Next", "Cancel");
    frame_text(c, d, title, text);
    snprintf(name, sizeof name, "RpLangG_%d", first);
    control(c, d, "RpLangGroup", "RadioButtonGroup", 25, 60, 145, 14 * ng, VIS | EN, "RpLangGroup", NULL, keep(c, name, NULL));
    prop(c, "RpLangGroup", "6");                    // English's, until RpLangApply settles the language
    for (int g = 1, k = 0; g <= 9; ++g) {
        if (count[g] == 0) continue;
        snprintf(val, sizeof val, "%d", g);
        snprintf(name, sizeof name, "LangGroup%d", g);
        if (!c->rtl) {
            rows_t *r = &c->radio;
            s_(c, r, "RpLangGroup"); i_(c, r, k + 1); s_(c, r, keep(c, val, NULL)); i_(c, r, 0);
            i_(c, r, k * 14); i_(c, r, 145); i_(c, r, 14); s_(c, r, T(c, keep(c, name, NULL))); n_(c, r);
        }
        ++k;
    }
    for (int g = 1; g <= 9; ++g) {
        if (count[g] == 0) continue;
        int next = 0;
        for (int h = g + 1; h <= 9 && next == 0; ++h) next = count[h] ? h : 0;
        char nname[24];
        snprintf(name, sizeof name, "RpLangG_%d", g);
        snprintf(nname, sizeof nname, "RpLangG_%d", next);
        const char *ctl = keep(c, name, NULL);
        control(c, d, ctl, "RadioButtonGroup", 180, 60, 165, 14 * count[g], VIS | EN, ctl, NULL, g == last ? "Back" : keep(c, nname, NULL));
        for (size_t li = 0, k = 0; li < c->nlang; ++li) {
            if (lang_group(c, li) != g) continue;
            if (k == 0) prop(c, ctl, lang_code(c, li));
            if (!c->rtl) {
                rows_t *r = &c->radio;
                const char *lname = c->ir->ui_langs[li].name ? c->ir->ui_langs[li].name : lang_code(c, li);
                s_(c, r, ctl); i_(c, r, (int32_t)k + 1); s_(c, r, lang_code(c, li)); i_(c, r, 0);
                i_(c, r, (int32_t)k * 14); i_(c, r, 165); i_(c, r, 14); s_(c, r, lname); n_(c, r);
            }
            ++k;
        }
        snprintf(cnd, sizeof cnd, "RpLangGroup = \"%d\"", g);
        cond(c, d, ctl, "Show", keep(c, cnd, NULL));
        event(c, d, "Next", "[RPLANGUAGE]", keep(c, "[", keep(c, ctl, "]")), keep(c, cnd, NULL), 0);
        snprintf(cnd, sizeof cnd, "RpLangGroup <> \"%d\"", g);
        cond(c, d, ctl, "Hide", keep(c, cnd, NULL));
    }
    control(c, d, "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), "Next");
    control(c, d, "Next", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Next"), "Cancel");
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "RpLangGroup");
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    event(c, d, "Next", "NewDialog", "RpWelcomeDlg", "NOT Installed", 2);
    event(c, d, "Next", "NewDialog", "RpMaintenanceDlg", "Installed", 2);
    dseq(c, d, "NOT Installed OR (NOT RESUME AND NOT Preselected)", 1225);
}

// The first page when the dialogs speak several languages: the heading and the line under it in
// every language (the line: up to three), a radio button per language (the automatic choice already selected), then the
// welcome page or, when installed, the maintenance page.
static void language_dlg(ctx_t *c) {
    const char *d = "RpLanguageDlg";
    const char *title = "", *text = "";
    for (size_t i = 0; i < c->nlang; ++i) {
        title = keep(c, title, keep(c, i ? " / " : "", text_for(c, "LanguageTitle", i)));
        text = keep(c, text, keep(c, i ? " / " : "", text_for(c, "LanguageText", i)));
    }
    // Four languages or more do not fit the two lines under the heading: the line is then the
    // language chosen in advance (the user's), whose texts are already in place.
    if (c->nlang > 3) text = T(c, "LanguageText");
    // Past RP_UI_LANG_ACTIONS the heading does not fit either: it too is the language's (RFC-0028).
    title = c->table ? TT(c, "LanguageTitle") : keep(c, "{\\RpTitle_en}", title);
    if (c->grouped) {
        grouped_page(c, title, text);
        return;
    }
    dialog(c, d, 370, 270, 3, "Lang", "Next", "Cancel");
    frame_text(c, d, title, text);
    int h = 14 * (int)c->nlang;
    // The buttons set RPLANGUAGEUI, which has a Property row (ICE34); RPLANGUAGE itself has none, so
    // that a missing one means "not given on the command line". Next copies it back first.
    control(c, d, "Lang", "RadioButtonGroup", 25, 60, 200, h, VIS | EN, "RPLANGUAGEUI", NULL, "Back");
    for (size_t i = 0; i < c->nlang && !c->rtl; ++i) {
        rows_t *r = &c->radio;
        const char *name = c->ir->ui_langs[i].name ? c->ir->ui_langs[i].name : lang_code(c, i);
        s_(c, r, "RPLANGUAGEUI"); i_(c, r, (int32_t)i + 1); s_(c, r, lang_code(c, i)); i_(c, r, 0);
        i_(c, r, (int32_t)i * 14); i_(c, r, 200); i_(c, r, 14); s_(c, r, name); n_(c, r);
    }
    control(c, d, "Back", "PushButton", 180, 243, 56, 17, VIS, NULL, T(c, "Back"), "Next");
    control(c, d, "Next", "PushButton", 236, 243, 56, 17, VIS | EN, NULL, T(c, "Next"), "Cancel");
    control(c, d, "Cancel", "PushButton", 304, 243, 56, 17, VIS | EN, NULL, T(c, "Cancel"), "Lang");
    event(c, d, "Cancel", "SpawnDialog", "RpCancelDlg", NULL, 1);
    event(c, d, "Next", "[RPLANGUAGE]", "[RPLANGUAGEUI]", NULL, 0);
    // Next: the texts and fonts of the chosen language first (language_rows), then the page.
    event(c, d, "Next", "NewDialog", "RpWelcomeDlg", "NOT Installed", 2);
    event(c, d, "Next", "NewDialog", "RpMaintenanceDlg", "Installed", 2);
    dseq(c, d, "NOT Installed OR (NOT RESUME AND NOT Preselected)", 1225);
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

// The status line's texts (ActionText) are set in InstallExecuteSequence as well: RPLANGUAGE
// reaches it from the dialogs (it is a secure property); a run without them (/qb) settles the
// language there, by the same rules (RpLangU_*, RpLangS_*, RpLangEn).
// Rows that put the chosen language's texts into the RpT_* properties (the oracle of RFC-0012: a
// property's value is shown as it is, so each text is formatted when it is put in):
//   - Property RpT_<key>_<code>: the text settled at build time (with its title style);
//     RpT_<key>: the English one, the default;
//   - InstallUISequence, before anything is shown: RPLANGUAGE from the command line, else the
//     first language whose LANGIDs hold UserLanguageID, then SystemLanguageID, else English; then
//     one type-51 action per text of that language (English needs them only for formatted texts);
//   - the language page's Next: the same for the language picked there.
// An event of the language page's Next (order 1: after RPLANGUAGE, before the next page), on its twin too.
static void page_event(ctx_t *c, const char *ev, const char *arg, const char *cnd) {
    for (int pass = 0; pass <= (int)c->rtl_any; ++pass) {
        c->rtl = pass == 1;
        event(c, "RpLanguageDlg", "Next", ev, arg, cnd, 1);
    }
    c->rtl = false;
}

// The set of dialogs: RpRtl while a right-to-left language is chosen, and the error dialog of
// that set (the engine shows the one ErrorDialog names).
static void rtl_rows(ctx_t *c) {
    if (c->rtl_any) {
        const char *rtl = "";
        for (size_t li = 1; li < c->nlang; ++li) {
            if (c->ir->ui_langs[li].rtl) rtl = keep(c, rtl, keep(c, *rtl ? " OR " : "", lang_cond(c, li)));
        }
        ca(c, "RpRtlSet", "RpRtl", "1");
        seq(c, "RpRtlSet", rtl, 21);
        ca(c, "RpRtlErr", "ErrorDialog", "RpErrorDlg_RTL");
        seq(c, "RpRtlErr", "RpRtl", 22);
        if (c->page) {
            const char *ltr = keep(c, "NOT (", keep(c, rtl, ")"));
            page_event(c, "[RpRtl]", "1", rtl);
            page_event(c, "[RpRtl]", "{}", ltr);
            page_event(c, "[ErrorDialog]", "RpErrorDlg_RTL", rtl);
            page_event(c, "[ErrorDialog]", "RpErrorDlg", ltr);
        }
    }
}

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
        // Each its own number (ICE82), in the order of [ui] languages, user languages before system
        // ones, ending at 17 and 18 as with one language: the first that matches sets RPLANGUAGE.
        int nl = (int)c->nlang - 1;
        seq(c, keep(c, name, NULL), keep(c, "NOT RPLANGUAGE AND (", keep(c, u, ")")), 18 - 2 * nl + (int)li);
        if (c->nseqs) c->seqs[c->nseqs - 1].exec = c->act;
        snprintf(name, sizeof name, "RpLangS_%s", L->code);
        ca(c, keep(c, name, NULL), "RPLANGUAGE", L->code);
        seq(c, keep(c, name, NULL), keep(c, "NOT RPLANGUAGE AND (", keep(c, sy, ")")), 18 - nl + (int)li);
        if (c->nseqs) c->seqs[c->nseqs - 1].exec = c->act;
    }
    ca(c, "RpLangEn", "RPLANGUAGE", "en");
    seq(c, "RpLangEn", "NOT RPLANGUAGE", 19);
    // With the status line's texts the language is settled in the execute sequence too: a run
    // without dialogs (/qb) never goes through the UI sequence.
    if (c->nseqs) c->seqs[c->nseqs - 1].exec = c->act;
    if (c->page) {
        prop(c, "RPLANGUAGEUI", "en");
        ca(c, "RpLangUi", "RPLANGUAGEUI", "[RPLANGUAGE]");
        seq(c, "RpLangUi", NULL, 20);
    }
    rtl_rows(c);
    for (size_t li = 0; li < c->nlang; ++li) {
        const char *code = lang_code(c, li), *cnd = lang_cond(c, li);
        const char *font = keep(c, "RpNormal_", code);
        if (li) {
            snprintf(name, sizeof name, "RpL_%s_font", code);
            ca(c, keep(c, name, NULL), "DefaultUIFont", font);
            seq(c, keep(c, name, NULL), cnd, lang_seq(c));
        }
        if (c->page) page_event(c, "[DefaultUIFont]", font, cnd);
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
            if (c->page) page_event(c, keep(c, "[", keep(c, target, "]")), arg, cnd);
            if (li || v == NULL) {
                snprintf(name, sizeof name, "RpL_%s_%d", code, ++n);
                ca(c, keep(c, name, NULL), target, arg);
                seq(c, keep(c, name, NULL), cnd, lang_seq(c));
                // The status line is formatted where the installation is done (observed, x55): the
                // property has to be set there too, in the execute sequence.
                if (strncmp(r->key, "Act_", 4) == 0 && c->nseqs) c->seqs[c->nseqs - 1].exec = true;
            }
        }
    }
}

// RFC-0028: past RP_UI_LANG_ACTIONS the texts are rows of RpText (Lang, Key, Text, Format) and the
// helper DLL's RpLangApply puts the chosen language's in place: at 19 in both sequences, where it
// first settles RPLANGUAGE by RP_UILANGS ("1042=ko;..." in the order of [ui] languages: the user's
// LANGID, then the system's, else English - as RpLangU_*, RpLangS_* and RpLangEn do), and on the
// language page's Next. Format 1: a text the installer formats first, as a type-51 action's target.
// DefaultUIFont is a row too. English's settled texts are also the properties' defaults.
static void language_table(ctx_t *c) {
    const char *map = "";
    for (size_t li = 1; li < c->nlang; ++li) {
        const rp_ir_ui_lang_t *L = &c->ir->ui_langs[li];
        for (size_t j = 0; j < L->langid_count; ++j) {
            char id[16];
            snprintf(id, sizeof id, "%u=", (unsigned)L->langids[j]);
            map = keep(c, map, keep(c, id, keep(c, L->code, ";")));
        }
    }
    prop(c, "RP_UILANGS", map);
    ca_dll(c, "RpLangApply", "RpLangApply");
    seq(c, "RpLangApply", NULL, 19);
    if (c->nseqs) c->seqs[c->nseqs - 1].exec = c->act;
    // A text may name a property AppSearch (50) fills: put in place again after it, as seven or
    // fewer languages' actions are (101 on). Same entry; RPLANGUAGE is settled by then.
    if (c->ir->search_count) {
        ca_dll(c, "RpLangApplySearch", "RpLangApply");
        seq(c, "RpLangApplySearch", NULL, 51);
        if (c->nseqs) c->seqs[c->nseqs - 1].exec = c->act;
    }
    if (c->page && !c->grouped) {
        prop(c, "RPLANGUAGEUI", "en");
        ca(c, "RpLangUi", "RPLANGUAGEUI", "[RPLANGUAGE]");
        seq(c, "RpLangUi", NULL, 20);
    }
    if (c->page) page_event(c, "DoAction", "RpLangApply", NULL);
    rtl_rows(c);
    rows_t *t = &c->rptext;
    for (size_t li = 0; li < c->nlang; ++li) {
        const char *code = lang_code(c, li);
        s_(c, t, code); s_(c, t, "DefaultUIFont"); s_(c, t, keep(c, "RpNormal_", code)); i_(c, t, 0);
        if (c->grouped && c->page) {        // the grouped page opens on the language's region and on it
            char g[16], ctl[24];
            snprintf(g, sizeof g, "%d", lang_group(c, li));
            snprintf(ctl, sizeof ctl, "RpLangG_%d", lang_group(c, li));
            s_(c, t, code); s_(c, t, "RpLangGroup"); s_(c, t, keep(c, g, NULL)); i_(c, t, 0);
            s_(c, t, code); s_(c, t, keep(c, ctl, NULL)); s_(c, t, code); i_(c, t, 0);
        }
        for (size_t k = 0; k < c->nreg; ++k) {
            const regtext_t *r = &c->reg[k];
            const char *src = r->src[li] ? r->src[li] : "";
            const char *pre = r->title ? keep(c, "{\\RpTitle_", keep(c, code, "}")) : "";
            const char *v = settle(c, src);
            const char *target = keep(c, "RpT_", r->key);
            const char *val = keep(c, pre, v ? v : src);
            if (li == 0 && v && val[0]) prop(c, target, val);
            s_(c, t, code); s_(c, t, target); s_(c, t, val); i_(c, t, v ? 0 : 1);
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
    rp_msi_wtable_t *tables = rp_mem_alloc(alloc, 13, sizeof *tables);
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
    c->table = c->nlang > RP_UI_LANG_ACTIONS;
    c->grouped = c->nlang > RP_UI_LANG_LIST;
    c->page = c->multi && ir->ui >= RP_UI_MINIMAL;
    c->details = in->details;
    c->details_dll = in->helper;
    c->images = in->control_images;
    c->image_count = in->control_image_count;
    c->bbimages = in->billboard_images;
    c->bbimage_count = in->billboard_image_count;
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
    rows_init(&c->acttext, "ActionText", acttext_cols, 3);
    rows_init(&c->rptext, "RpText", rptext_cols, 4);
    rows_init(&c->billboard, "Billboard", billboard_cols, 4);
    rows_init(&c->bbcontrol, "BBControl", bbcontrol_cols, 9);

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
    // The status line's texts, when every language has them (English here, another in its pack;
    // a language without them keeps Windows Installer's own, which follow Windows). With several languages the text is a
    // property the chosen language fills, as every other text.
    bool act = true;
    for (size_t i = 0; i < c->nlang; ++i) act = act && rp_ui_text_in_pack("Action.InstallFiles", lang_code(c, i));
    c->act = act;
    for (size_t i = 0; act && i < sizeof action_texts / sizeof action_texts[0]; ++i) {
        const action_text_t *a = &action_texts[i];
        const char *src[RP_UI_LANG_MAX] = { 0 };
        for (size_t li = 0; li < c->nlang; ++li) {
            const char *t = strcmp(lang_code(c, li), "en") == 0 ? NULL
                          : rp_langpack_text(rp_langpack_get(lang_code(c, li), NULL), "installer", keep(c, "Action.", a->action));
            src[li] = t ? t : a->en;
        }
        s_(c, &c->acttext, a->action);
        s_(c, &c->acttext, c->multi ? reg_text(c, keep(c, "Act_", a->action), src, false) : src[0]);
        s_(c, &c->acttext, a->tmpl);
    }
    // The refusals (lower.c's RP_RefuseDowngrade at 30, RP_RefuseOld at 31) in the chosen language:
    // past RP_UI_LANG_ACTIONS RpLangApply at 19 puts them in place; with fewer, lower.c gives each
    // language an error action of its own.
    if (c->table && ir->ui) {
        if (ir->downgrade_message == NULL) (void)T(c, "NewerInstalled");
        if (ir->refuse_below && ir->refuse_message == NULL) (void)T(c, "OlderRefused");
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
    for (size_t i = 0; i < c->nlang && ir->ui_lang_count; ++i) c->rtl_any |= ir->ui_langs[i].rtl;
    c->rtl_any = c->rtl_any && c->multi;
    for (int pass = 0; pass <= (int)c->rtl_any; ++pass) {     // the dialogs, then their mirrored twins
    c->rtl = pass == 1;
    c->ncustom = 0;                         // the twins' own texts get the same keys
    cancel_dlg(c);
    error_dlg(c);
    files_in_use_dlg(c);
    progress_and_exits(c);
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
            else if (strcmp(p, "RpCustomizeDlg") == 0) customize_dlg(c, back, next, NULL);
            else ready_dlg(c, back);
        }
        rp_mem_free(alloc, pages);
    }
    // Every set: an installed product is asked what to do (basic has no other page). With features
    // to choose, Change leads to the feature tree - the set's own page, or one made for this.
    bool change = ir->ui == RP_UI_FEATURES || choosable(ir);
    if (change && ir->ui != RP_UI_FEATURES) customize_dlg(c, NULL, "", "Install");
    maintenance_dlg(c, change);
    if (ir->ui >= RP_UI_INSTALLDIR || change) prop(c, "_RpBrowseProperty", dir);
    }
    c->rtl = false;
    merge_rows(c);
    if (c->table) language_table(c);
    else if (c->multi) language_rows(c);

    rows_t *all[] = { &c->dialog, &c->control, &c->event, &c->condition, &c->mapping, &c->style, &c->uitext, &c->binary, &c->radio, &c->combo, &c->listbox, &c->acttext, &c->rptext, &c->billboard, &c->bbcontrol };
    size_t nt = 0;
    for (size_t i = 0; i < sizeof all / sizeof *all; ++i) {
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
    ui->lang_table = c->table;
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
        rows_t *all[] = { &c->dialog, &c->control, &c->event, &c->condition, &c->mapping, &c->style, &c->uitext, &c->binary, &c->radio, &c->combo, &c->listbox, &c->acttext, &c->rptext, &c->billboard, &c->bbcontrol };
        for (size_t i = 0; i < sizeof all / sizeof *all; ++i) rp_mem_free(alloc, all[i]->cells);
        for (size_t i = 0; i < c->nstr; ++i) rp_mem_free(alloc, c->strings[i]);
        rp_mem_free(alloc, c->strings);
        rp_mem_free(alloc, c->props);
        rp_mem_free(alloc, c->seqs);
        rp_mem_free(alloc, c->cas);
        rp_mem_free(alloc, c->reg);
        rp_mem_free(alloc, c->mcond);
        rp_mem_free(alloc, c->mctl);
        rp_mem_free(alloc, c);
    }
    rp_mem_free(alloc, ui->tables);
    rp_mem_free(alloc, ui);
}

// ---- license RTF -----------------------------------------------------------------------------

static const char *const face_names[][2] = {
    { "맑은 고딕", "Malgun Gothic" }, { "굴림", "Gulim" }, { "굴림체", "GulimChe" }, { "돋움", "Dotum" },
    { "돋움체", "DotumChe" }, { "바탕", "Batang" }, { "바탕체", "BatangChe" }, { "궁서", "Gungsuh" },
    { "궁서체", "GungsuhChe" }, { "나눔고딕", "NanumGothic" }, { "나눔명조", "NanumMyeongjo" },
    { "나눔바른고딕", "NanumBarunGothic" }, { "メイリオ", "Meiryo" }, { "游ゴシック", "Yu Gothic" },
    { "游明朝", "Yu Mincho" }, { "ＭＳ ゴシック", "MS Gothic" }, { "ＭＳ 明朝", "MS Mincho" },
    { "ＭＳ Ｐゴシック", "MS PGothic" }, { "微软雅黑", "Microsoft YaHei" }, { "宋体", "SimSun" }, { "黑体", "SimHei" },
    { "微軟正黑體", "Microsoft JhengHei" }, { "新細明體", "PMingLiU" },
};

const char *rp_ui_face_ascii(const char *face) {
    if (face == NULL) return NULL;
    bool ascii = true;
    for (const char *p = face; *p && ascii; ++p) ascii = (unsigned char)*p < 0x80;
    if (ascii) return face;
    for (size_t k = 0; k < sizeof face_names / sizeof face_names[0]; ++k) {
        if (strcmp(face, face_names[k][0]) == 0) return face_names[k][1];
    }
    return NULL;
}

// One code point of UTF-8 at t[*i], advanced past it (a stray byte stands for itself).
static uint32_t utf8_next(const uint8_t *t, size_t len, size_t *i) {
    uint32_t cp = t[*i];
    size_t k = 1;
    if (cp >= 0xF0 && *i + 3 < len) cp = (cp & 7) << 18 | (t[*i + 1] & 63u) << 12 | (t[*i + 2] & 63u) << 6 | (t[*i + 3] & 63u), k = 4;
    else if (cp >= 0xE0 && *i + 2 < len) cp = (cp & 15) << 12 | (t[*i + 1] & 63u) << 6 | (t[*i + 2] & 63u), k = 3;
    else if (cp >= 0xC0 && *i + 1 < len) cp = (cp & 31) << 6 | (t[*i + 1] & 63u), k = 2;
    *i += k;
    return cp;
}

// A code point as RTF text: ASCII as it is (\\, \{, \} escaped), the rest as \uN? (UTF-16 units).
static size_t rtf_char(char *s, size_t cap, uint32_t cp) {
    if (cp == '\\' || cp == '{' || cp == '}') return (size_t)snprintf(s, cap, "\\%c", (char)cp);
    if (cp < 0x80) return (size_t)snprintf(s, cap, "%c", (char)cp);
    if (cp < 0x10000) return (size_t)snprintf(s, cap, "\\u%d?", (int)(int16_t)cp);
    uint32_t v = cp - 0x10000;      // a surrogate pair, each as its own \uN
    return (size_t)snprintf(s, cap, "\\u%d?\\u%d?", (int)(int16_t)(0xD800 + (v >> 10)), (int)(int16_t)(0xDC00 + (v & 0x3FF)));
}

proven_err_t rp_ui_text_to_rtf(proven_allocator_t alloc, const uint8_t *text, size_t len, const char *face, uint8_t **out,
                               size_t *out_len) {
    size_t flen = face ? strlen(face) : 0;
    size_t cap = 256 + flen * 12 + len * 10, n = 0;
    char *s = rp_mem_alloc(alloc, cap, 1);
    if (s == NULL) return PROVEN_ERR_NOMEM;
    n += (size_t)snprintf(s, cap, "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fnil ");
    for (size_t j = 0; j < flen;) n += rtf_char(s + n, cap - n, utf8_next((const uint8_t *)face, flen, &j));
    n += (size_t)snprintf(s + n, cap - n, ";}}\\fs18 ");
    size_t i = 0;
    if (len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) i = 3;
    while (i < len && n + 16 < cap) {
        uint32_t cp = utf8_next(text, len, &i);
        if (cp == '\r') continue;
        if (cp == '\n') n += (size_t)snprintf(s + n, cap - n, "\\par\n");
        else n += rtf_char(s + n, cap - n, cp);
    }
    n += (size_t)snprintf(s + n, cap - n, "}");
    *out = (uint8_t *)s;
    *out_len = n;
    return PROVEN_OK;
}
