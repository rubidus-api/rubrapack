#ifndef RUBRAPACK_IR_H
#define RUBRAPACK_IR_H

// include/rubrapack/ir.h - the checked package model built from a `.rpk` AST (RFC-0002 section 9).
// Everything here has passed the table/key/type/path checks; back ends (MSI lowering) never look
// at the source text again. Strings are UTF-8 and owned by the IR.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/srcdiag.h"
#include "rubrapack/toml.h"

typedef enum { RP_ARCH_X64, RP_ARCH_ARM64, RP_ARCH_X86 } rp_arch_t;

typedef struct {
    char    *id;
    char    *title;
    char    *description;       // may be NULL
    int32_t  level;
    bool     hidden;
    char    *parent;            // feature ID or NULL
    bool     implicit;          // the default `Main` feature (G2)
    bool     required;          // RFC-0013 A3: cannot be set to "not installed" in the tree
    bool     follow_parent;     // RFC-0013 A3: installed where its parent is
    char    *when;              // RFC-0013 A2: an MSI condition, or NULL
    rp_pos_t pos;
} rp_ir_feature_t;

typedef struct {
    char    *id;
    char    *base;              // known folder name (ProgramFiles, ...) or NULL when `parent` is set
    char    *parent;            // dir ID or NULL
    char   **parts;             // relative path components below base/parent (at least one)
    size_t   part_count;
    char    *feature;           // may be NULL
    bool     implicit;          // a sub folder created for [files.*] matches below a wildcard
    bool     guard;             // RFC-0012 V6: an existing folder must be owned by administrators
    rp_pos_t pos;
} rp_ir_dir_t;

typedef struct {
    char    *id;                // logical ID
    char    *dir;               // dir ID
    char    *source;            // path as written (relative to the .rpk directory, `/`)
    char    *source_path;       // path to open (joined with the .rpk directory)
    char    *name;              // target file name
    uint64_t size;
    bool     any_arch, keep, vital;
    uint16_t pe_machine;        // 0 when the file is not a PE program (checked in rp_ir_build)
    bool     pe_is_dll;
    bool     msi_only;          // msi-only = true: left out of an MSIX (RFC-0009 M6)
    char    *feature;           // resolved (G2): never NULL
    char    *component_guid;    // user-fixed GUID or NULL
    char    *when;              // RFC-0013 A2: an MSI condition, or NULL
    rp_pos_t pos;
} rp_ir_file_t;

typedef struct {
    char    *id;
    char    *dir;               // dir ID
    char    *name;              // folder name
    bool     keep;              // left behind at uninstall
    char    *feature;           // resolved (G2)
    rp_pos_t pos;
} rp_ir_folder_t;

typedef enum { RP_REG_STRING, RP_REG_EXPAND, RP_REG_DWORD, RP_REG_BINARY, RP_REG_MULTI, RP_REG_QWORD } rp_reg_type_t;
typedef enum { RP_ROOT_HKMU = -1, RP_ROOT_HKCR = 0, RP_ROOT_HKCU = 1, RP_ROOT_HKLM = 2 } rp_reg_root_t;

// [registry.ID] (RFC-0004): one value (or, with remove, a value or key removed at install).
typedef struct {
    char         *id;
    rp_reg_root_t root;
    char         *key;          // literal
    char         *name;         // literal; NULL = the key's default value
    rp_reg_type_t type;
    char         *value;        // string/expand: formatted text; dword: decimal; binary: hex digits;
                                // qword: 16 hex digits (written by the helper DLL, RFC-0001 9.6)
    char        **items;        // multi: formatted strings
    size_t        item_count;
    bool          remove, keep, view32;
    bool          msi_only;     // msi-only = true: left out of an MSIX (RFC-0009 M6)
    char         *with_file;    // file ID whose component carries it, or NULL (own component)
    char         *feature;      // resolved (G2) when with_file is NULL
    char         *when;         // RFC-0013 A2, own component only
    rp_pos_t      pos;
} rp_ir_registry_t;

// [shortcut.ID] (RFC-0004): lives in its target file's component.
typedef struct {
    char    *id;
    char    *dir;               // dir ID, or a known folder: Programs, Desktop, StartMenu, Startup
    char    *name;              // literal, without ".lnk"
    char    *target_file;       // file ID
    char    *args;              // formatted, or NULL
    char    *description;       // literal, or NULL
    char    *working_dir;       // dir ID, or NULL
    char    *icon_source;       // .ico to open (RFC-0013 A1), or NULL: the target's own icon
    char    *when;              // RFC-0013 A2: then the shortcut has its own component
    char    *icon_shown;        // as written
    rp_pos_t pos;
} rp_ir_shortcut_t;

// [remove.ID] (RFC-0004): files matching `name` (or the folder itself) removed at install,
// uninstall or both. [copy.ID]: a second copy of an installed file.
typedef struct {
    char    *id;
    char    *dir;               // dir ID
    char    *name;              // pattern with * and ?, or NULL = the folder itself
    int      mode;              // 1 install, 2 uninstall, 3 both (RemoveFile.InstallMode)
    char    *feature;           // resolved (G2)
    rp_pos_t pos;
} rp_ir_remove_t;

// [ini.ID] (RFC-0004): one key of an INI file.
typedef struct {
    char    *id;
    char    *dir;               // dir ID
    char    *file;              // INI file name (literal)
    char    *section, *key;     // literal
    char    *value;             // formatted; NULL for remove
    int      mode;              // 0 set, 1 add (to a comma list), 2 remove (at install)
    char    *feature;           // resolved (G2)
    char    *when;              // RFC-0013 A2: an MSI condition, or NULL
    rp_pos_t pos;
} rp_ir_ini_t;

// [require.ID] (RFC-0004): a launch condition. [search.ID]: AppSearch into a public property.
typedef struct {
    char    *id;
    char    *condition;         // MSI conditional expression
    char    *message;           // formatted
    rp_pos_t pos;
} rp_ir_require_t;

typedef enum { RP_SEARCH_REGISTRY, RP_SEARCH_FILE, RP_SEARCH_DIR, RP_SEARCH_COMPONENT } rp_search_kind_t;

typedef struct {
    char            *id;
    char            *property;      // upper case
    rp_search_kind_t kind;
    rp_reg_root_t    root;          // registry
    char            *key, *name;    // registry (literal; name NULL = default value)
    bool             view32;        // registry
    char            *base;          // file/dir: known folder (ProgramFiles, System, ...)
    char            *path;          // file/dir: relative path below base with '\', or NULL
    char            *file_name;     // file
    char            *min_version;   // file, or NULL
    char            *component_guid;// component
    bool             fills_dir;     // property is a dir the user may change (RFC-0012 V5): the folder
                                    // found becomes its default unless the command line set it
    rp_pos_t         pos;
} rp_ir_search_t;

// [service.ID] (RFC-0004): a Windows service run from an installed exe.
typedef struct {
    char    *id;
    char    *file;              // file ID of the exe
    char    *name;              // service name (literal)
    char    *display_name;      // literal, or NULL = name
    char    *description;       // literal, or NULL
    int      start;             // 2 auto, 3 demand, 4 disabled (ServiceInstall.StartType)
    int      account;           // 0 LocalSystem, 1 LocalService, 2 NetworkService
    char    *args;              // formatted, or NULL
    bool     start_on_install;
    rp_pos_t pos;
} rp_ir_service_t;

// [merge.ID] (RFC-0016 3): a merge module (.msm) merged into the MSI: its TARGETDIR becomes `dir`,
// its components go into `feature`, its cabinet is embedded as a cabinet of its own.
typedef struct {
    char    *id;
    char    *source;            // path of the .msm (joined with the source's folder)
    char    *shown;             // as written, for messages
    char    *dir;               // dir ID
    char    *feature;           // resolved (G2)
    rp_pos_t pos;
} rp_ir_merge_t;

// [font.ID] (RFC-0004): registers a font file of this package installed in the Fonts folder.
typedef struct {
    char    *id;
    char    *file;              // file ID
    char    *title;             // NULL: the engine reads it from a TrueType/OpenType file
    rp_pos_t pos;
} rp_ir_font_t;

// [assoc.ID] (RFC-0010 N4): a file type opened by an installed program. MSI: HKCR values in the
// program's component (the extension's default = prog-id; prog-id, DefaultIcon, shell\open\command);
// MSIX: uap3:FileTypeAssociation of the application whose executable it is (grouped by prog-id).
typedef struct {
    char    *id;
    char    *extension;         // ".ext", lower case
    char    *prog_id;
    char    *description;       // literal, or NULL
    char    *target_file;       // file ID of the program
    char    *icon_file;         // file ID, or NULL (MSI: the program's first icon)
    char    *args;              // formatted; default "\"%1\""
    rp_pos_t pos;
} rp_ir_assoc_t;

// [protocol.ID] (RFC-0010 N4): a URI scheme opened by an installed program. MSI: HKCR\<name>
// (URL Protocol, DefaultIcon, shell\open\command); MSIX: uap3:Protocol.
typedef struct {
    char    *id;
    char    *name;              // scheme, lower case
    char    *description;       // literal, or NULL
    char    *target_file;
    char    *args;              // formatted; default "\"%1\""
    rp_pos_t pos;
} rp_ir_protocol_t;

// [msix-extension.ID] (RFC-0010 N4): what only an MSIX has; left out of an MSI.
typedef enum { RP_MSIX_EXT_ALIAS, RP_MSIX_EXT_STARTUP, RP_MSIX_EXT_FIREWALL, RP_MSIX_EXT_COM, RP_MSIX_EXT_TOAST,
               RP_MSIX_EXT_CONTEXT_MENU } rp_msix_ext_kind_t;
typedef struct {
    char              *id;
    rp_msix_ext_kind_t kind;
    char              *app;         // [msix-app.*] ID, or NULL = the first
    char              *alias;       // alias: "name.exe"
    char              *task_id;     // startup-task: default the table's ID
    char              *display;     // startup-task: display name, or NULL
    bool               enabled;     // startup-task (default true)
    char              *file;        // firewall: the program's file ID, or NULL = the application's
    bool               outbound;    // firewall: direction = "out"
    bool               udp;         // firewall: protocol = "udp" (else TCP)
    unsigned           port_min, port_max;  // firewall: local ports, 0 = any
    char              *profile;     // firewall: "all" (default), "domain", "private", "public"
    char              *clsid;       // com-server, toast, context-menu: "{GUID}", upper case
    char              *threading;   // com-server (a DLL), context-menu: "STA", "MTA", "Both", "Neutral"
    char              *args;        // com-server (an exe), toast: plain text, or NULL
    char              *verb;        // context-menu: the verb's ID (default the table's ID)
    char             **types;       // context-menu: ".ext" or "*"
    size_t             type_count;
    rp_pos_t           pos;
} rp_ir_msix_ext_t;

// [permission.ID] (RFC-0004): an SDDL security descriptor set on a folder, file or registry value.
typedef struct {
    char    *id;
    int      kind;              // 0 dir, 1 file, 2 registry
    char    *target;            // the dir, file or registry ID
    char    *sddl;
    char    *feature;           // dir: resolved (G2)
    rp_pos_t pos;
} rp_ir_permission_t;

// [env.ID] (RFC-0004): a system environment variable.
typedef struct {
    char    *id;
    char    *name;              // literal
    char    *value;             // formatted
    int      mode;              // 0 set, 1 append, 2 prepend
    bool     keep;
    char    *feature;           // resolved (G2)
    char    *when;              // RFC-0013 A2: an MSI condition, or NULL
    rp_pos_t pos;
} rp_ir_env_t;

typedef struct {
    char    *id;
    char    *source_file;       // file ID
    char    *dir;               // dir ID
    char    *name;              // target name; the source's name when omitted
    rp_pos_t pos;
} rp_ir_copy_t;

// A text written for one language: `text-ko = "..."` (RFC-0012).
typedef struct {
    char    *lang;              // "ko", "ja", ...
    char    *text;              // formatted
} rp_ir_ltext_t;

typedef struct {
    char    *lang;
    char   **labels;            // one per value of the control
} rp_ir_llabels_t;

// The languages of the dialogs (RFC-0012): English always, first and the default; [ui] languages
// adds the others.
enum { RP_UI_LANG_MAX = 8, RP_UI_LANGID_MAX = 16 };
typedef struct {
    char     code[4];           // "en", "ko", ...
    char    *name;              // [ui] name-xx: the language page's label, or NULL (built in)
    char    *font;              // [ui] font-xx, or NULL (built in)
    uint16_t langids[RP_UI_LANGID_MAX];     // chosen when UserLanguageID or SystemLanguageID is one of them
    size_t   langid_count;
    char    *license_source;    // [ui] license-xx: path to open, or NULL
    char    *license_shown;     // as written
    rp_pos_t pos;
} rp_ir_ui_lang_t;

typedef struct {
    char    *id;                // a text ID of the dialog sets (rp_ui_text_known)
    char    *text;              // formatted, every language; may be NULL when only text-xx is given
    rp_ir_ltext_t *by_lang;     // text-xx
    size_t   by_lang_count;
    rp_pos_t pos;
} rp_ir_ui_text_t;

// [dialog.ID] and [dialog-control.ID] (RFC-0005 K4): an author's page in the built-in flow.
typedef struct {
    char    *id;                // the Dialog key
    char    *title, *description;   // banner heading (formatted) and the line under it (or NULL)
    rp_ir_ltext_t *title_by_lang, *description_by_lang;     // title-xx, description-xx
    size_t   title_by_lang_count, description_by_lang_count;
    char    *after;             // a built-in page (RpWelcomeDlg, RpLicenseDlg, RpInstallDirDlg,
                                // RpCustomizeDlg) or another [dialog.*]
    rp_pos_t pos;
} rp_ir_dialog_t;

enum { RP_DC_TEXT, RP_DC_CHECKBOX, RP_DC_EDIT, RP_DC_RADIO, RP_DC_COMBO };

typedef struct {
    char    *id;                // the Control key (unique across all tables)
    char    *dialog;            // [dialog.*] ID
    int      type;              // RP_DC_*
    int      x, y, width, height;   // dialog units, inside the body (y 45..234)
    char    *text;              // label (text, checkbox), formatted
    char    *property;          // public property (checkbox, edit, radio, combo)
    char   **values;            // radio, combo: the property values
    char   **labels;            // their labels (the values when omitted)
    size_t   value_count;
    rp_ir_ltext_t *text_by_lang;        // text-xx
    size_t   text_by_lang_count;
    rp_ir_llabels_t *labels_by_lang;    // labels-xx
    size_t   labels_by_lang_count;
    rp_pos_t pos;
} rp_ir_dialog_control_t;

typedef struct {
    char    *id;                // public property name (upper case)
    char    *value;
    bool     secure, hidden;    // SecureCustomProperties / MsiHiddenProperties
    rp_pos_t pos;
} rp_ir_property_t;

// A do/undo pair run from an installed exe (RFC-0003): `do` at install and repair, `undo` at
// removal; the lowering adds conditions, order and rollback twins.
typedef struct {
    char    *id;
    char    *run_file;          // file ID of the exe (from run = "file:ID")
    char    *do_args, *undo_args;
    char    *check_args;        // may be NULL
    rp_pos_t pos;
} rp_ir_action_t;

// [msix-app.ID] (RFC-0009, RFC-0010 N1: several).
typedef struct {
    char    *id, *exe;                  // exe: a [file] ID
    char    *display, *description;     // may be NULL
    rp_ir_ltext_t *display_by_lang, *description_by_lang;       // display-name-xx, description-xx (RFC-0016 3)
    size_t   display_by_lang_count, description_by_lang_count;
    char    *logo[3];                   // Square150x150, Square44x44, StoreLogo (as written), or NULL
    char    *logo_path[3];              // to open (joined with the .rpk directory)
    rp_pos_t pos;
} rp_ir_msix_app_t;

// A table an MSIX cannot hold (RFC-0009 M6): an error when building .msix, unless msi-only = true.
typedef struct {
    char    *kind, *id;
    rp_pos_t pos;
} rp_ir_msix_block_t;

typedef struct {
    // [package]
    char     *name, *summary_name, *manufacturer;
    char     *version;          // as written, validated
    uint16_t  version_parts[4];
    size_t    version_count;
    rp_arch_t arch;
    char     *upgrade_code;     // upper case, braces
    char     *product_code;     // NULL = derive
    uint16_t  language;         // 1033 or 1042
    bool      reboot_suppress;
    int       scope;            // 0 machine, 1 user, 2 dual (per-user by default, ALLUSERS=1 for machine)
    char     *downgrade_message;
    char     *refuse_below;     // refuse-upgrade-below (versions below it are refused), or NULL
    char     *refuse_message;   // may be NULL
    int       compress;         // -1 none, 0-9 MSZIP level
    bool      cab_external;     // cab = "external": cabinets next to the package
    uint64_t  cab_max;          // cab-max-size in bytes of input per cabinet, 0 = one cabinet

    rp_ir_feature_t *features;
    size_t           feature_count;
    rp_ir_dir_t     *dirs;
    size_t           dir_count;
    rp_ir_file_t    *files;
    size_t           file_count;
    rp_ir_folder_t  *folders;
    size_t           folder_count;
    rp_ir_property_t *properties;
    size_t            property_count;
    rp_ir_action_t   *actions;
    size_t            action_count;
    rp_ir_registry_t *registries;
    size_t            registry_count;
    rp_ir_shortcut_t *shortcuts;
    size_t            shortcut_count;
    rp_ir_remove_t   *removes;
    size_t            remove_count;
    rp_ir_copy_t     *copies;
    size_t            copy_count;
    rp_ir_env_t      *envs;
    size_t            env_count;
    rp_ir_ini_t      *inis;
    size_t            ini_count;
    rp_ir_require_t  *requires;
    size_t            require_count;
    rp_ir_search_t   *searches;
    size_t            search_count;
    rp_ir_service_t  *services;
    size_t            service_count;
    rp_ir_font_t     *fonts;
    size_t            font_count;
    rp_ir_assoc_t    *assocs;               // in ID order
    size_t            assoc_count;
    rp_ir_protocol_t *protocols;            // in ID order
    size_t            protocol_count;
    rp_ir_msix_ext_t *msix_exts;            // in ID order
    size_t            msix_ext_count;
    // P4 dialogs (RFC-0005)
    int               ui;                   // RP_UI_* (rubrapack/ui.h)
    char             *license_source;       // path to open (joined with the .rpk directory), or NULL
    char             *license_shown;        // as written
    char             *banner_source;        // [ui] banner BMP, or NULL
    char             *ui_install_dir;       // [ui] install-dir (dir ID), or NULL
    char             *ui_launch_file;       // [ui] launch = "file:ID" (RFC-0013 A5), or NULL
    char             *ui_launch_args;       // [ui] launch-args (formatted), or NULL
    bool              ui_launch_default;    // [ui] launch-checked (default true)
    rp_ir_ui_lang_t   ui_langs[RP_UI_LANG_MAX];     // RFC-0012: [0] is English; one entry = English only
    size_t            ui_lang_count;
    rp_ir_ui_text_t  *ui_texts;
    size_t            ui_text_count;
    rp_ir_dialog_t   *dialogs;              // in ID order
    size_t            dialog_count;
    rp_ir_dialog_control_t *dialog_controls;    // in ID order
    size_t            dialog_control_count;
    rp_ir_permission_t *permissions;
    size_t            permission_count;
    rp_ir_merge_t    *merges;
    size_t            merge_count;
    // [arp]
    bool      arp_no_modify, arp_no_repair;
    char     *arp_help, *arp_about;     // may be NULL
    char     *arp_icon_source, *arp_icon_shown;     // [arp] icon: .ico to open / as written (RFC-0013 A1)
    // [msix] and [msix-app.ID] (RFC-0009; used only when the output is .msix)
    bool      has_msix;
    char     *msix_identity_name, *msix_publisher, *msix_publisher_display, *msix_min_version;
    char     *msix_display;             // [msix] display-name (the package's; default the name)
    rp_ir_ltext_t *msix_display_by_lang, *msix_publisher_display_by_lang;   // -xx (RFC-0016 3: resources.pri)
    size_t    msix_display_by_lang_count, msix_publisher_display_by_lang_count;
    char     *msix_appinstaller_uri, *msix_package_uri;  // RFC-0016 3: an .appinstaller beside the output
    int       msix_update_hours;                          // HoursBetweenUpdateChecks (default 24)
    bool      msix_update_prompt, msix_update_blocks, msix_update_background;
    rp_pos_t  msix_pos;
    rp_ir_msix_app_t *msix_apps;                // in source order
    size_t    msix_app_count;
    rp_ir_msix_block_t *msix_blocks;            // tables an MSIX cannot carry, without msi-only = true
    size_t    msix_block_count;
    proven_allocator_t alloc;
} rp_ir_t;

typedef struct {
    const char *name;
    const char *value;
} rp_define_t;

typedef struct {
    const char        *source_dir;      // directory of the .rpk file ("." when none)
    const rp_define_t *defines;         // -D NAME=VALUE, already checked for duplicates
    size_t             define_count;
    const char        *arch;            // --arch, or NULL
    const char        *compress;        // --compress, or NULL
    const char        *output;          // the file being built (refused inside a glob), or NULL
    bool               nfc;             // --nfc: names inside the package in NFC (RFC-0006 L3)
} rp_ir_options_t;

// Checks the document and builds the model. On any error the diagnostics say why, the model is
// empty, and PROVEN_ERR_INVALID_FORMAT is returned.
[[nodiscard]] proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt,
                                       rp_ir_t *ir, rp_srcdiags_t *diags);
void rp_ir_free(rp_ir_t *ir);

// A stable text dump of the model for golden tests.
[[nodiscard]] proven_err_t rp_ir_dump(const rp_ir_t *ir, proven_allocator_t alloc, uint8_t **out, size_t *len);

#endif // RUBRAPACK_IR_H
