#ifndef RUBRAPACK_IR_H
#define RUBRAPACK_IR_H

// include/rubrapack/ir.h - the checked package model built from a `.toml` AST (RFC-0002 section 9).
// Everything here has passed the table/key/type/path checks; back ends (MSI lowering) never look
// at the source text again. Strings are UTF-8 and owned by the IR.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "proven/u8str.h"
#include "rubrapack/str.h"
#include "rubrapack/srcdiag.h"
#include "rubrapack/toml.h"

typedef enum { RP_ARCH_X64, RP_ARCH_ARM64, RP_ARCH_X86 } rp_arch_t;

typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t title;
    proven_u8str_view_t description;       // may be NULL
    int32_t  level;
    bool     hidden;
    proven_u8str_view_t parent;            // feature ID or NULL
    bool     implicit;          // the default `Main` feature (G2)
    bool     required;          // RFC-0013 A3: cannot be set to "not installed" in the tree
    bool     follow_parent;     // RFC-0013 A3: installed where its parent is
    bool     no_advertise;      // RFC-0031 B8: advertise = "disallow" (Feature.Attributes DisallowAdvertise)
    proven_u8str_view_t when;              // RFC-0013 A2: an MSI condition, or NULL
    proven_u8str_view_t default_when;      // x48: a level above 1, but installed by default when this condition holds
    rp_pos_t pos;
} rp_ir_feature_t;

typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t base;              // known folder name (ProgramFiles, ...) or NULL when `parent` is set
    proven_u8str_view_t parent;            // dir ID or NULL
    char   **parts;             // relative path components below base/parent (at least one)
    size_t   part_count;
    proven_u8str_view_t feature;           // may be NULL
    bool     implicit;          // a sub folder created for [files.*] matches below a wildcard
    bool     guard;             // RFC-0012 V6: an existing folder must be owned by administrators
    rp_pos_t pos;
} rp_ir_dir_t;

typedef struct {
    proven_u8str_view_t id;                // logical ID
    proven_u8str_view_t dir;               // dir ID
    proven_u8str_view_t source;            // path as written (relative to the .toml directory, `/`)
    proven_u8str_view_t source_path;       // path to open (joined with the .toml directory)
    proven_u8str_view_t name;              // target file name
    uint64_t size;
    bool     any_arch, keep, vital;
    uint16_t pe_machine;        // 0 when the file is not a PE program (checked in rp_ir_build)
    bool     pe_is_dll;
    bool     msi_only;          // msi-only = true: left out of an MSIX (RFC-0009 M6)
    proven_u8str_view_t feature;           // resolved (G2): never NULL
    proven_u8str_view_t component_guid;    // user-fixed GUID or NULL
    proven_u8str_view_t when;              // RFC-0013 A2: an MSI condition, or NULL
    proven_u8str_view_t isolate_for;       // RFC-0031 B4: file ID of the program this DLL is isolated for, or NULL
    // RFC-0031 B9: an assembly (MsiAssembly, MsiAssemblyName). A .NET one names itself in its CLI
    // metadata; a Win32 one in its manifest, a [file] of its own (`manifest`) that joins the DLL's
    // component (`manifest_of` on it). `assembly_for`: the program it is private to (else global).
    enum { RP_ASM_NONE, RP_ASM_DOTNET, RP_ASM_WIN32 } assembly;
    proven_u8str_view_t assembly_for;      // file ID, or NULL
    proven_u8str_view_t manifest;          // RP_ASM_WIN32: the manifest's file ID
    proven_u8str_view_t manifest_of;       // on the manifest: the DLL's file ID (set by the checks)
    char    *asm_names[6][2];   // the identity, name and value pairs (set by the checks; NULL ends)
    // A file rubrapack makes itself (a shared Win32 assembly's catalog, rp_ir_add_sxs_catalog):
    // its bytes, owned, in place of source_path (NULL).
    uint8_t *data;
    size_t   data_len;
    rp_pos_t pos;
} rp_ir_file_t;

typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t dir;               // dir ID
    proven_u8str_view_t name;              // folder name
    bool     keep;              // left behind at uninstall
    proven_u8str_view_t feature;           // resolved (G2)
    rp_pos_t pos;
} rp_ir_folder_t;

typedef enum { RP_REG_STRING, RP_REG_EXPAND, RP_REG_DWORD, RP_REG_BINARY, RP_REG_MULTI, RP_REG_QWORD } rp_reg_type_t;
typedef enum { RP_ROOT_HKMU = -1, RP_ROOT_HKCR = 0, RP_ROOT_HKCU = 1, RP_ROOT_HKLM = 2 } rp_reg_root_t;

// [registry.ID] (RFC-0004): one value (or, with remove, a value or key removed at install).
typedef struct {
    proven_u8str_view_t id;
    rp_reg_root_t root;
    proven_u8str_view_t key;          // literal
    proven_u8str_view_t name;         // literal; NULL = the key's default value
    rp_reg_type_t type;
    proven_u8str_view_t value;        // string/expand: formatted text; dword: decimal; binary: hex digits;
                                // qword: 16 hex digits (written by the helper DLL, RFC-0001 9.6)
    char        **items;        // multi: formatted strings
    size_t        item_count;
    bool          remove, keep, view32;
    bool          purge;        // remove = true, on = "uninstall": the key with everything below it, at a real removal (helper DLL)
    bool          users_all;    // users = "all": every user's HKCU, when the removal is told so (RFC-0030)
    bool          msi_only;     // msi-only = true: left out of an MSIX (RFC-0009 M6)
    proven_u8str_view_t with_file;    // file ID whose component carries it, or NULL (own component)
    proven_u8str_view_t feature;      // resolved (G2) when with_file is NULL
    proven_u8str_view_t when;         // RFC-0013 A2, own component only
    rp_pos_t      pos;
} rp_ir_registry_t;

// [shortcut.ID] (RFC-0004): lives in its target file's component.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t dir;               // dir ID, or a known folder: Programs, Desktop, StartMenu, Startup
    proven_u8str_view_t name;              // literal, without ".lnk"
    proven_u8str_view_t target_file;       // file ID
    proven_u8str_view_t args;              // formatted, or NULL
    proven_u8str_view_t description;       // literal, or NULL
    proven_u8str_view_t working_dir;       // dir ID, or NULL
    proven_u8str_view_t icon_source;       // .ico to open (RFC-0013 A1), or NULL: the target's own icon
    proven_u8str_view_t when;              // RFC-0013 A2: then the shortcut has its own component
    proven_u8str_view_t icon_shown;        // as written
    bool     advertise;         // RFC-0031 B8: an advertised shortcut (Target = the feature; installs on demand)
    rp_pos_t pos;
} rp_ir_shortcut_t;

// [billboard.ID] (RFC-0031 B10): a picture shown on the progress page while its feature's files
// install. One .bmp each; MSI only.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t image_source;      // the .bmp to open (joined with the .toml dir)
    proven_u8str_view_t image_shown;       // the path as written
    proven_u8str_view_t feature;           // the feature whose installation shows it
    int32_t  order;             // display order within the feature (0 = by ID)
    rp_pos_t pos;
} rp_ir_billboard_t;

// [remove.ID] (RFC-0004): files matching `name` (or the folder itself) removed at install,
// uninstall or both. [copy.ID]: a second copy of an installed file.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t dir;               // dir ID
    proven_u8str_view_t name;              // pattern with * and ?, or NULL = the folder itself
    int      mode;              // 1 install, 2 uninstall, 3 both (RemoveFile.InstallMode)
    bool     keep_on_upgrade;   // upgrade = false: the removal part skips upgrades (helper DLL)
    bool     recursive;         // recursive = true: the folder with everything in it, at a real removal (helper DLL)
    bool     users_all;         // users = "all": every user's same folder, when the removal is told so (RFC-0030)
    proven_u8str_view_t feature;           // resolved (G2)
    rp_pos_t pos;
} rp_ir_remove_t;

// [ini.ID] (RFC-0004): one key of an INI file.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t dir;               // dir ID
    proven_u8str_view_t file;              // INI file name (literal)
    proven_u8str_view_t section, key;     // literal
    proven_u8str_view_t value;             // formatted; NULL for remove
    int      mode;              // 0 set, 1 add (to a comma list), 2 remove (at install)
    proven_u8str_view_t feature;           // resolved (G2)
    proven_u8str_view_t when;              // RFC-0013 A2: an MSI condition, or NULL
    bool     msi_only;          // msi-only = true: left out of an MSIX
    rp_pos_t pos;
} rp_ir_ini_t;

// [require.ID] (RFC-0004): a launch condition. [search.ID]: AppSearch into a public property.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t condition;         // MSI conditional expression
    proven_u8str_view_t message;           // formatted
    rp_pos_t pos;
} rp_ir_require_t;

typedef enum { RP_SEARCH_REGISTRY, RP_SEARCH_FILE, RP_SEARCH_DIR, RP_SEARCH_COMPONENT } rp_search_kind_t;

typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t property;      // upper case
    rp_search_kind_t kind;
    rp_reg_root_t    root;          // registry
    proven_u8str_view_t key, name;    // registry (literal; name NULL = default value)
    bool             view32;        // registry
    proven_u8str_view_t base;          // file/dir: known folder (ProgramFiles, System, ...)
    proven_u8str_view_t path;          // file/dir: relative path below base with '\', or NULL
    proven_u8str_view_t file_name;     // file
    proven_u8str_view_t min_version;   // file, or NULL
    proven_u8str_view_t component_guid;// component
    bool             fills_dir;     // property is a dir the user may change (RFC-0012 V5): the folder
                                    // found becomes its default unless the command line set it
    rp_pos_t         pos;
} rp_ir_search_t;

// [service.ID] (RFC-0004): a Windows service run from an installed exe.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t file;              // file ID of the exe
    proven_u8str_view_t name;              // service name (literal)
    proven_u8str_view_t display_name;      // literal, or NULL = name
    proven_u8str_view_t description;       // literal, or NULL
    int      start;             // 2 auto, 3 demand, 4 disabled (ServiceInstall.StartType)
    int      account;           // 0 LocalSystem, 1 LocalService, 2 NetworkService
    proven_u8str_view_t args;              // formatted, or NULL
    bool     start_on_install;
    rp_pos_t pos;
} rp_ir_service_t;

// [merge.ID] (RFC-0016 3): a merge module (.msm) merged into the MSI: its TARGETDIR becomes `dir`,
// its components go into `feature`, its cabinet is embedded as a cabinet of its own.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t source;            // path of the .msm (joined with the source's folder)
    proven_u8str_view_t shown;             // as written, for messages
    proven_u8str_view_t dir;               // dir ID
    proven_u8str_view_t feature;           // resolved (G2)
    char   **config;            // "Name=value" answers for a configurable module (RFC-0017 C)
    size_t   config_count;
    bool     msi_only;          // msi-only = true: left out of an MSIX (RFC-0019)
    rp_pos_t pos;
} rp_ir_merge_t;

// [font.ID] (RFC-0004): registers a font file of this package installed in the Fonts folder.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t file;              // file ID
    proven_u8str_view_t title;             // NULL: the engine reads it from a TrueType/OpenType file
    rp_pos_t pos;
} rp_ir_font_t;

// [assoc.ID] (RFC-0010 N4): a file type opened by an installed program. MSI: HKCR values in the
// program's component (the extension's default = prog-id; prog-id, DefaultIcon, shell\open\command);
// MSIX: uap3:FileTypeAssociation of the application whose executable it is (grouped by prog-id).
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t extension;         // ".ext", lower case
    proven_u8str_view_t prog_id;
    proven_u8str_view_t description;       // literal, or NULL
    proven_u8str_view_t target_file;       // file ID of the program
    proven_u8str_view_t icon_file;         // file ID, or NULL (MSI: the program's first icon)
    proven_u8str_view_t logo;              // MSIX: the type's PNG (as written), or NULL; an MSI does not use it
    proven_u8str_view_t logo_path;         // to open (joined with the .toml directory)
    proven_u8str_view_t args;              // formatted; default "\"%1\""
    proven_u8str_view_t content_type;      // "image/png": the extension's Content Type, or NULL
    proven_u8str_view_t perceived_type;    // "image", "text", "audio", "video", "compressed", "document", "system", "application", or NULL
    bool     no_default;        // default = false: the program is offered ("Open with"), the type is not taken
    rp_pos_t pos;
} rp_ir_assoc_t;

// [protocol.ID] (RFC-0010 N4): a URI scheme opened by an installed program. MSI: HKCR\<name>
// (URL Protocol, DefaultIcon, shell\open\command); MSIX: uap3:Protocol.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t name;              // scheme, lower case
    proven_u8str_view_t description;       // literal, or NULL
    proven_u8str_view_t target_file;
    proven_u8str_view_t args;              // formatted; default "\"%1\""
    rp_pos_t pos;
} rp_ir_protocol_t;

// [com.ID] (RFC-0022): a COM class served by a program or DLL of this package. MSI: HKCR values in
// the server's component (CLSID, its server, ProgID, AppID, TypeLib); MSIX: com:ComServer and
// com:ProgId (through an [msix-extension] of kind com-server made from it).
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t file;              // file ID of the .exe or .dll that serves the class
    bool     exe;               // the server is an .exe (LocalServer32), else a DLL (InprocServer32)
    proven_u8str_view_t clsid;             // "{GUID}", upper case
    proven_u8str_view_t description;       // literal, or NULL (the product's name)
    proven_u8str_view_t threading;         // a DLL's ThreadingModel: "Apartment" (default), "Free", "Both", "Neutral"
    proven_u8str_view_t args;              // an exe's arguments (formatted), or NULL
    proven_u8str_view_t prog_id;           // or NULL
    proven_u8str_view_t app_id;            // "{GUID}", or NULL (surrogate without it: the class ID)
    bool     surrogate;         // a DLL run in dllhost (AppID DllSurrogate)
    proven_u8str_view_t typelib;           // "{LIBID}", or NULL
    proven_u8str_view_t typelib_version;   // "major.minor" in hex digits (default "1.0")
    proven_u8str_view_t typelib_file;      // file ID holding the type library (default: the server)
    bool     msi_only;
    rp_pos_t pos;
} rp_ir_com_t;

// [handler.ID] (RFC-0024): an Explorer handler for file types, served by a [com.*] DLL class of the
// package. MSI: HKCR\.ext\ShellEx values (preview, thumbnail), the PreviewHandlers list and the
// prevhost AppID for a preview handler, PropertySystem\PropertyHandlers (property, per machine).
// MSIX: desktop2:DesktopPreviewHandler, ThumbnailHandler, DesktopPropertyHandler in a file type
// association.
typedef enum { RP_HANDLER_PREVIEW, RP_HANDLER_THUMBNAIL, RP_HANDLER_PROPERTY } rp_handler_kind_t;
typedef struct {
    proven_u8str_view_t id;
    rp_handler_kind_t kind;
    proven_u8str_view_t clsid;        // a [com.*] class, "{GUID}" upper case
    char            **types;        // ".ext", lower case
    size_t            type_count;
    proven_u8str_view_t description;  // a preview handler's name, or NULL (the [com]'s description)
    bool              msi_only;
    rp_pos_t          pos;
} rp_ir_handler_t;

// [msix-extension.ID] (RFC-0010 N4): what only an MSIX has; left out of an MSI.
typedef enum { RP_MSIX_EXT_ALIAS, RP_MSIX_EXT_STARTUP, RP_MSIX_EXT_FIREWALL, RP_MSIX_EXT_COM, RP_MSIX_EXT_TOAST,
               RP_MSIX_EXT_CONTEXT_MENU, RP_MSIX_EXT_APP_EXTENSION, RP_MSIX_EXT_APP_EXTENSION_HOST, RP_MSIX_EXT_APP_SERVICE,
               RP_MSIX_EXT_SHARE_TARGET, RP_MSIX_EXT_AUTOPLAY } rp_msix_ext_kind_t;
typedef struct {
    proven_u8str_view_t id;
    rp_msix_ext_kind_t kind;
    proven_u8str_view_t app;         // [msix-app.*] ID, or NULL = the first
    proven_u8str_view_t alias;       // alias: "name.exe"
    proven_u8str_view_t task_id;     // startup-task: default the table's ID
    proven_u8str_view_t display;     // startup-task: display name, or NULL
    bool               enabled;     // startup-task (default true)
    proven_u8str_view_t file;        // firewall: the program's file ID, or NULL = the application's
    bool               outbound;    // firewall: direction = "out"
    bool               udp;         // firewall: protocol = "udp" (else TCP)
    unsigned           port_min, port_max;  // firewall: local ports, 0 = any
    proven_u8str_view_t profile;     // firewall: "all" (default), "domain", "private", "public"
    proven_u8str_view_t clsid;       // com-server, toast, context-menu: "{GUID}", upper case
    proven_u8str_view_t threading;   // com-server (a DLL), context-menu: "STA", "MTA", "Both", "Neutral"
    proven_u8str_view_t args;        // com-server (an exe), toast: plain text, or NULL
    proven_u8str_view_t prog_id;     // com-server made from a [com.*]: its ProgID, or NULL
    proven_u8str_view_t verb;        // context-menu: the verb's ID (default the table's ID)
    char             **types;       // context-menu, share-target: ".ext" or "*"
    size_t             type_count;
    // RFC-0031 B15-B18 (written into an MSIX only, RFC-0031 section 10):
    proven_u8str_view_t name;        // app-extension: the category it serves; app-service: the service's name
    proven_u8str_view_t ext_id;      // app-extension: its Id (default the table's ID)
    proven_u8str_view_t folder;      // app-extension: the public folder, or NULL
    proven_u8str_view_t description; // app-extension, share-target: or NULL
    proven_u8str_view_t entry;       // app-service: an activatable class, or NULL = the application's program
    bool               multi;       // app-service: SupportsMultipleInstances
    proven_u8str_view_t action;      // autoplay: the action shown ("Import pictures")
    char             **names;       // app-extension-host: categories; share-target: data formats;
    size_t             name_count;  //   autoplay: content events
    char             **events;      // autoplay: device events (need class: the IHWEventHandler)
    size_t             event_count;
    rp_pos_t           pos;
} rp_ir_msix_ext_t;

// [permission.ID] (RFC-0004): an SDDL security descriptor set on a folder, file or registry value.
typedef struct {
    proven_u8str_view_t id;
    int      kind;              // 0 dir, 1 file, 2 registry
    proven_u8str_view_t target;            // the dir, file or registry ID
    proven_u8str_view_t sddl;
    proven_u8str_view_t feature;           // dir: resolved (G2)
    rp_pos_t pos;
} rp_ir_permission_t;

// [env.ID] (RFC-0004): a system environment variable.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t name;              // literal
    proven_u8str_view_t value;             // formatted
    int      mode;              // 0 set, 1 append, 2 prepend
    bool     keep;
    proven_u8str_view_t feature;           // resolved (G2)
    proven_u8str_view_t when;              // RFC-0013 A2: an MSI condition, or NULL
    bool     msi_only;          // msi-only = true: left out of an MSIX
    rp_pos_t pos;
} rp_ir_env_t;

typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t source_file;       // file ID
    proven_u8str_view_t dir;               // dir ID
    proven_u8str_view_t name;              // target name; the source's name when omitted
    bool     msi_only;          // msi-only = true: left out of an MSIX
    rp_pos_t pos;
} rp_ir_copy_t;

// A text written for one language: `text-ko = "..."` (RFC-0012).
typedef struct {
    proven_u8str_view_t lang;              // "ko", "ja", ...
    proven_u8str_view_t text;              // formatted
} rp_ir_ltext_t;

// RFC-0031 wave 1: MSI tables with no MSIX counterpart.
// [move.ID]: MoveFile - files moved or copied at installation from a folder (a dir ID, or a
// property a [search] fills) to one of the package's folders.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t from_dir, from_prop;  // one of them
    proven_u8str_view_t name;                  // file name, wildcards allowed
    proven_u8str_view_t to_dir;                // dir ID
    proven_u8str_view_t new_name;              // or NULL: the same name
    bool     copy;                  // copy = true: the source stays
    proven_u8str_view_t with_file;             // the component it goes with ("file:ID"), or NULL: a component of its own
    proven_u8str_view_t feature;               // with its own component; NULL = the default feature
    rp_pos_t pos;
} rp_ir_move_t;

// [odbc-driver.ID] and [odbc-source.ID]: ODBCDriver / ODBCDataSource and their attributes.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t file;                  // file ID of the driver DLL
    proven_u8str_view_t setup;                 // file ID of its setup DLL, or NULL (the driver itself)
    proven_u8str_view_t description;           // the driver's name, as ODBC lists it
    char   **attributes;            // "Key=Value"
    size_t   attribute_count;
    rp_pos_t pos;
} rp_ir_odbc_driver_t;

typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t driver;                // an [odbc-driver] ID, or NULL with driver-name
    proven_u8str_view_t driver_name;           // a driver installed some other way, or NULL
    proven_u8str_view_t name;                  // the data source's name
    bool     user;                  // registration = "user" (default "machine")
    char   **attributes;
    size_t   attribute_count;
    proven_u8str_view_t with_file;             // the component, when driver is not one of this package
    rp_pos_t pos;
} rp_ir_odbc_source_t;

// [publish.ID]: PublishComponent - a qualified component other programs find with
// MsiProvideQualifiedComponent.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t file;                  // file ID: its component is published
    proven_u8str_view_t category;              // {GUID}
    proven_u8str_view_t qualifier;
    proven_u8str_view_t data;                  // AppData, or NULL
    rp_pos_t pos;
} rp_ir_publish_t;

// [other-copy.ID] (plan 2026-10-17-menu-list-and-hide): the same program installed another way - an
// MSIX for the installing user, or another MSI - found by the preflight before a first install,
// which warns (Yes / No) or refuses. MSI only; an MSIX build ignores it.
typedef struct {
    proven_u8str_view_t id;
    proven_u8str_view_t msix;            // the package's identity name, with publisher; or NULL
    proven_u8str_view_t publisher;       // its publisher ("CN=..."), with msix
    proven_u8str_view_t upgrade_code;    // another MSI's upgrade code, upper case; or NULL
    bool           refuse;          // action = "refuse" (default "warn")
    proven_u8str_view_t message;         // formatted ([1] this product, [2] the copy found); or NULL
    rp_ir_ltext_t *message_by_lang; // message-xx
    size_t         message_by_lang_count;
    rp_pos_t       pos;
} rp_ir_other_t;

// [menu.ID] (plan 2026-10-09): an item of Explorer's right-click menu that starts a program.
// The classic menu (Windows 10; "Show more options" on Windows 11) gets registry verbs; the
// Windows 11 menu gets a class of rubrapack's menu DLL in a package with identity.
enum { RP_MENU_EACH, RP_MENU_ONE, RP_MENU_SINGLE };
enum { RP_MENU_VALUES = 6 };       // registry values one item writes at one place of the classic menu, at most
typedef struct {
    proven_u8str_view_t id;
    char         **on;              // ".ext", "*", "folder", "background", "drive", "assoc:ID"
    size_t         on_count;
    proven_u8str_view_t text;            // or NULL when every language has text-xx
    rp_ir_ltext_t *text_by_lang;    // text-xx
    size_t         text_by_lang_count;
    proven_u8str_view_t target_file;     // file ID of the program; NULL = a sub-menu
    proven_u8str_view_t icon_file;       // file ID, or NULL (the program's own icon)
    proven_u8str_view_t args;            // formatted; default "\"%1\""
    proven_u8str_view_t list;            // formatted, or NULL: stands for %* with %L = a file of the paths, when they do not fit a command line
    proven_u8str_view_t hide_when;       // "HKCU\\key\\value" or "HKLM\\...": the Windows 11 item is hidden while that value exists; or NULL
    proven_u8str_view_t parent;          // [menu.*] ID of the sub-menu this item is in, or NULL
    proven_u8str_view_t clsid;           // "{GUID}" of its Windows 11 class (top-level items), made from the upgrade code and the ID
    int            multi;           // RP_MENU_*
    bool           extended;        // classic menu: only with Shift
    bool           windows11;       // also in the Windows 11 menu (default true)
    rp_pos_t       pos;
} rp_ir_menu_t;

typedef struct {
    proven_u8str_view_t lang;
    char   **labels;            // one per value of the control
} rp_ir_llabels_t;

// The languages of the dialogs (RFC-0012): English always, first and the default; [ui] languages
// adds the others. Up to RP_UI_LANG_ACTIONS (English with seven) each text of each language is a
// set-property action; past it the texts are rows of the RpText table that the helper DLL puts in
// place (RFC-0028).
// The language page lists them in one column of RP_UI_LANG_LIST radio buttons.
enum { RP_UI_LANG_MAX = 64, RP_UI_LANG_ACTIONS = 8, RP_UI_LANG_LIST = 12, RP_UI_LANGID_MAX = 16 };
typedef struct {
    char     code[8];           // "en", "ko", "zh_TW", ...
    proven_u8str_view_t name;              // [ui] name-xx: the language page's label, or NULL (built in)
    proven_u8str_view_t font;              // [ui] font-xx, else the language's built-in face, else Segoe UI
    uint16_t langids[RP_UI_LANGID_MAX];     // chosen when UserLanguageID or SystemLanguageID is one of them
    size_t   langid_count;
    bool     rtl;               // the language reads right to left (its pack's X-Rubrapack-Direction)
    int      group;             // its region on a grouped language page (RFC-0028): 1-8, 9 other
    proven_u8str_view_t license_source;    // [ui] license-xx: path to open, or NULL
    proven_u8str_view_t license_shown;     // as written
    rp_pos_t pos;
} rp_ir_ui_lang_t;

typedef struct {
    proven_u8str_view_t id;                // a text ID of the dialog sets (rp_ui_text_known)
    proven_u8str_view_t text;              // formatted, every language; may be NULL when only text-xx is given
    rp_ir_ltext_t *by_lang;     // text-xx
    size_t   by_lang_count;
    rp_pos_t pos;
} rp_ir_ui_text_t;

// [dialog.ID] and [dialog-control.ID] (RFC-0005 K4): an author's page in the built-in flow.
typedef struct {
    proven_u8str_view_t id;                // the Dialog key
    proven_u8str_view_t title, description;   // banner heading (formatted) and the line under it (or NULL)
    rp_ir_ltext_t *title_by_lang, *description_by_lang;     // title-xx, description-xx
    size_t   title_by_lang_count, description_by_lang_count;
    proven_u8str_view_t after;             // a built-in page (RpWelcomeDlg, RpLicenseDlg, RpInstallDirDlg,
                                // RpCustomizeDlg) or another [dialog.*]
    rp_pos_t pos;
} rp_ir_dialog_t;

enum { RP_DC_TEXT, RP_DC_CHECKBOX, RP_DC_EDIT, RP_DC_RADIO, RP_DC_COMBO, RP_DC_LISTBOX, RP_DC_BITMAP };

typedef struct {
    proven_u8str_view_t id;                // the Control key (unique across all tables)
    proven_u8str_view_t dialog;            // [dialog.*] ID
    int      type;              // RP_DC_*
    int      x, y, width, height;   // dialog units, inside the body (y 45..234)
    proven_u8str_view_t text;              // label (text, checkbox), formatted
    proven_u8str_view_t property;          // public property (checkbox, edit, radio, combo)
    char   **values;            // radio, combo, listbox: the property values
    char   **labels;            // their labels (the values when omitted)
    size_t   value_count;
    proven_u8str_view_t image_source;      // bitmap: the .bmp to open (joined with the .toml dir), or NULL
    proven_u8str_view_t image_shown;       // bitmap: the path as written
    rp_ir_ltext_t *text_by_lang;        // text-xx
    size_t   text_by_lang_count;
    rp_ir_llabels_t *labels_by_lang;    // labels-xx
    size_t   labels_by_lang_count;
    rp_pos_t pos;
} rp_ir_dialog_control_t;

typedef struct {
    proven_u8str_view_t id;                // public property name (upper case)
    proven_u8str_view_t value;
    bool     secure, hidden;    // SecureCustomProperties / MsiHiddenProperties
    rp_pos_t pos;
} rp_ir_property_t;

// A do/undo pair run from an installed exe (RFC-0003): `do` at install and repair, `undo` at
// removal; the lowering adds conditions, order and rollback twins.
// RFC-0031 B5/B6: or one action as written - an installed exe (run, args), an installed DLL's
// function (dll, entry) or a DLL carried in the package (binary, entry) - in one phase, placed
// before or after a standard action or at a sequence number.
enum { RP_ACT_PAIR, RP_ACT_EXE, RP_ACT_DLL, RP_ACT_BINARY };
enum { RP_PHASE_IMMEDIATE, RP_PHASE_DEFERRED, RP_PHASE_COMMIT, RP_PHASE_ROLLBACK };
typedef struct {
    proven_u8str_view_t id;
    int      kind;              // RP_ACT_*
    proven_u8str_view_t run_file;          // file ID of the exe (from run = "file:ID")
    proven_u8str_view_t do_args, undo_args;
    proven_u8str_view_t check_args;        // may be NULL
    proven_u8str_view_t args;              // a single exe action's arguments (formatted), or NULL
    proven_u8str_view_t dll_file;          // RP_ACT_DLL: file ID
    proven_u8str_view_t binary_shown, binary_source;  // RP_ACT_BINARY: as written / to open
    proven_u8str_view_t entry;             // the DLL's function
    int      phase;             // RP_PHASE_*
    int      as_user;           // -1 the package's default (system per machine, user per user), 0 system, 1 user
    proven_u8str_view_t when;              // condition, or NULL
    proven_u8str_view_t data;              // CustomActionData (formatted) of a deferred action, or NULL
    proven_u8str_view_t before, after;    // a standard action's name, or NULL
    int32_t  sequence;          // 0 = not given
    bool     ignore_exit;       // the result is not checked
    rp_pos_t pos;
} rp_ir_action_t;

// [msix-app.ID] (RFC-0009, RFC-0010 N1: several).
typedef struct {
    proven_u8str_view_t id, exe;                  // exe: a [file] ID
    proven_u8str_view_t display, description;     // may be NULL
    rp_ir_ltext_t *display_by_lang, *description_by_lang;       // display-name-xx, description-xx (RFC-0016 3)
    size_t   display_by_lang_count, description_by_lang_count;
    char    *logo[3];                   // Square150x150, Square44x44, StoreLogo (as written), or NULL
    char    *logo_path[3];              // to open (joined with the .toml directory)
    proven_u8str_view_t background;                // background-color: "transparent" or "#RRGGBB" (RFC-0018), or NULL
    bool     hidden;                    // hidden = true: no entry in the Start menu (AppListEntry="none")
    rp_pos_t pos;
} rp_ir_msix_app_t;

// [msix-dependency.ID] (RFC-0018): a framework package the MSIX needs (PackageDependency).
typedef struct {
    proven_u8str_view_t id, name, publisher, min_version;
    rp_pos_t pos;
} rp_ir_msix_dep_t;

// A table an MSIX cannot hold (RFC-0009 M6): an error when building .msix, unless msi-only = true.
typedef struct {
    proven_u8str_view_t kind, id;
    rp_pos_t pos;
} rp_ir_msix_block_t;

typedef struct {
    // [package]
    proven_u8str_view_t name, summary_name, manufacturer;
    proven_u8str_view_t version;          // as written, validated
    uint16_t  version_parts[4];
    size_t    version_count;
    rp_arch_t arch;
    proven_u8str_view_t upgrade_code;     // upper case, braces; a merge module's GUID ([module] id)
    bool      module;           // [module]: a merge module (.msm), not a package
    proven_u8str_view_t product_code;     // NULL = derive
    uint16_t  language;         // 1033 or 1042
    bool      reboot_suppress;
    bool      no_cleanup;       // [package] cleanup = false: no cleanup task (RFC-0026)
    bool      no_preflight;     // [package] preflight = false: no checks before the installation goes on
    int       close_programs;   // 0 ask (fail without UI), 1 always, 2 never
    int       extra_files;      // at a removal, files not installed in the product's folders: 0 ask (fail without UI), 1 keep, 2 delete
    proven_u8str_view_t parent;           // [package] parent: the main product's UpgradeCode (an add-on, x46), or NULL
    bool      remove_addons;    // [package] remove-addons = true: a real removal removes the add-ons too (x46)
    char      replaces[16][39]; // [package] replaces: other products' UpgradeCodes this package removes (x47)
    size_t    replace_count;
    int       scope;            // 0 machine, 1 user, 2 dual (per-user by default, ALLUSERS=1 for machine)
    int32_t   instances;        // RFC-0031 B12: [package] instances = N (>=2): N-1 named instance transforms embedded; 0/1 = none
    proven_u8str_view_t downgrade_message;
    proven_u8str_view_t refuse_below;     // refuse-upgrade-below (versions below it are refused), or NULL
    proven_u8str_view_t refuse_message;   // may be NULL
    int       compress;         // -1 none, 0-9 MSZIP level
    bool      cab_external;     // cab = "external": cabinets next to the package
    uint64_t  cab_max;          // cab-max-size in bytes of input per cabinet, 0 = one cabinet

    rp_ir_feature_t *features;
    size_t           feature_count;
    rp_ir_dir_t     *dirs;
    size_t           dir_count;
    size_t          *dir_order;         // dirs with an ID, by ID then place (rp_ir_find_dir)
    size_t           dir_order_len, dir_order_of;   // its length; dir_count when it was made
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
    rp_ir_com_t      *coms;                 // [com.*], in ID order
    size_t            com_count;
    rp_ir_handler_t  *handlers;             // [handler.*], in ID order
    size_t            handler_count;
    rp_ir_msix_ext_t *msix_exts;            // in ID order
    size_t            msix_ext_count;
    rp_ir_menu_t     *menus;                // [menu.*], in ID order
    size_t            menu_count;
    rp_ir_other_t    *others;               // [other-copy.*]
    size_t            other_count;
    rp_ir_move_t     *moves;                // [move.*]
    size_t            move_count;
    rp_ir_odbc_driver_t *odbc_drivers;      // [odbc-driver.*]
    size_t            odbc_driver_count;
    rp_ir_odbc_source_t *odbc_sources;      // [odbc-source.*]
    size_t            odbc_source_count;
    rp_ir_publish_t  *publishes;            // [publish.*]
    size_t            publish_count;
    // An item for the Windows 11 menu: an MSI then carries rubrapack's menu DLL and an identity
    // package beside the program `menu_exe` (a file ID), whose folder is the package's location.
    bool              menu_package;
    proven_u8str_view_t menu_exe;
    proven_u8str_view_t menu_publisher;       // from the options: the signed package's Publisher, or NULL
    // P4 dialogs (RFC-0005)
    int               ui;                   // RP_UI_* (rubrapack/ui.h)
    proven_u8str_view_t license_source;       // path to open (joined with the .toml directory), or NULL
    proven_u8str_view_t license_shown;        // as written
    proven_u8str_view_t banner_source;        // [ui] banner BMP, or NULL
    proven_u8str_view_t ui_embedded_source;    // RFC-0031 B11: [ui] embedded author UI DLL path, or NULL
    proven_u8str_view_t ui_embedded_shown;     // ... as written
    proven_u8str_view_t ui_install_dir;       // [ui] install-dir (dir ID), or NULL
    proven_u8str_view_t ui_launch_file;       // [ui] launch = "file:ID" (RFC-0013 A5), or NULL
    proven_u8str_view_t ui_launch_args;       // [ui] launch-args (formatted), or NULL
    bool              ui_launch_default;    // [ui] launch-checked (default true)
    bool              ui_save_log;          // [ui] save-log (default true): the last page saves the log
    bool              ui_details;           // [ui] details (default true): the progress page shows what is being done
    rp_ir_ui_lang_t   ui_langs[RP_UI_LANG_MAX];     // RFC-0012: [0] is English; one entry = English only
    size_t            ui_lang_count;
    rp_ir_ui_text_t  *ui_texts;
    size_t            ui_text_count;
    rp_ir_dialog_t   *dialogs;              // in ID order
    size_t            dialog_count;
    rp_ir_dialog_control_t *dialog_controls;    // in ID order
    size_t            dialog_control_count;
    rp_ir_billboard_t *billboards;              // RFC-0031 B10: in ID order
    size_t            billboard_count;
    rp_ir_permission_t *permissions;
    size_t            permission_count;
    rp_ir_merge_t    *merges;
    size_t            merge_count;
    // [arp]
    bool      arp_no_modify, arp_no_repair;
    bool      arp_no_remove;    // Installed apps: Uninstall off, removal through Modify (the dialogs)
    rp_pos_t  arp_no_remove_pos;
    proven_u8str_view_t arp_help, arp_about;     // may be NULL
    proven_u8str_view_t arp_icon_source, arp_icon_shown;     // [arp] icon: .ico to open / as written (RFC-0013 A1)
    proven_u8str_view_t arp_logo_source, arp_logo_shown;     // [arp] logo: a .png for the menu identity package's logos (RFC-0031 L7)
    // [msix] and [msix-app.ID] (RFC-0009; used only when the output is .msix)
    bool      has_msix;
    proven_u8str_view_t msix_identity_name, msix_publisher, msix_publisher_display, msix_min_version;
    proven_u8str_view_t msix_display;             // [msix] display-name (the package's; default the name)
    rp_ir_ltext_t *msix_display_by_lang, *msix_publisher_display_by_lang;   // -xx (RFC-0016 3: resources.pri)
    size_t    msix_display_by_lang_count, msix_publisher_display_by_lang_count;
    proven_u8str_view_t msix_appinstaller_uri, msix_package_uri;  // RFC-0016 3: an .appinstaller beside the output
    int       msix_update_hours;                          // HoursBetweenUpdateChecks (default 24)
    bool      msix_update_prompt, msix_update_blocks, msix_update_background;
    rp_pos_t  msix_pos;
    rp_ir_msix_app_t *msix_apps;                // in source order
    size_t    msix_app_count;
    char    **msix_caps;                        // [msix] capabilities (RFC-0018), as written
    size_t    msix_cap_count;
    rp_pos_t  msix_caps_pos;
    bool      msix_no_fs_virt, msix_no_reg_virt;    // file-system-/registry-virtualization = false
    rp_ir_msix_dep_t *msix_deps;                // [msix-dependency.*], in source order
    proven_u8str_view_t msix_main, msix_main_publisher;  // main-package (and its publisher): an optional package (RFC-0019)
    bool      msix_modification;                // modification = true: a modification package of msix_main
    bool      msix_framework;                   // framework = true: a framework package other packages depend on (RFC-0031 B19)
    bool      msix_external;                    // external-location = true: the identity of a program installed elsewhere (RFC-0031 B20)
    char    **msix_symbols;                     // [msix] symbols: the .pdb files of a .msixupload (RFC-0031 B22), paths to read
    bool      has_winget;                       // [winget] (RFC-0031 B28): what a winget manifest says besides the package
    proven_u8str_view_t wg_id, wg_url, wg_license, wg_desc;                      // identifier, installer-url, license, short-description
    proven_u8str_view_t wg_publisher_url, wg_package_url, wg_license_url, wg_moniker;
    char    **wg_tags;
    size_t    wg_tag_count;
    rp_ir_ltext_t *wg_desc_by_lang;             // short-description-xx
    size_t    wg_desc_by_lang_count;
    rp_pos_t  wg_pos;
    bool      has_psf;                          // [msix-psf] (RFC-0031 B26): the Package Support Framework
    proven_u8str_view_t psf_files, psf_config;           // its folder of PSF files and its config.json, paths to read
    proven_u8str_view_t psf_launcher;                     // launcher = "...": the launcher's file name, or NULL (by architecture)
    rp_pos_t  psf_pos;
    size_t    msix_symbol_count;
    bool      msix_no_langpacks;                // language-packs = false: a bundle keeps every language in its packages
    size_t    msix_dep_count;
    rp_ir_msix_block_t *msix_blocks;            // tables an MSIX cannot carry, without msi-only = true
    size_t    msix_block_count;
    proven_allocator_t alloc;
} rp_ir_t;

typedef struct {
    proven_u8str_view_t name;
    proven_u8str_view_t value;
} rp_define_t;

typedef struct {
    proven_u8str_view_t source_dir;      // directory of the .toml file ("." when none)
    const rp_define_t *defines;         // -D NAME=VALUE, already checked for duplicates
    size_t             define_count;
    proven_u8str_view_t arch;            // --arch, or NULL
    proven_u8str_view_t compress;        // --compress, or NULL
    proven_u8str_view_t output;          // the file being built (refused inside a glob), or NULL
    bool               nfc;             // --nfc: names inside the package in NFC (RFC-0006 L3)
    // The Windows 11 menu of an MSI (rp_ir_t menu_package): the menu DLL and the identity package as
    // files on disk, made by the build command from a first model; both or neither.
    proven_u8str_view_t menu_dll, menu_pkg;
    proven_u8str_view_t menu_publisher;  // the identity package is signed: its Publisher (else NULL: unsigned)
} rp_ir_options_t;

// Checks the document and builds the model. On any error the diagnostics say why, the model is
// empty, and PROVEN_ERR_INVALID_FORMAT is returned.
[[nodiscard]] proven_err_t rp_ir_build(proven_allocator_t alloc, const rp_tdoc_t *doc, const rp_ir_options_t *opt,
                                       rp_ir_t *ir, rp_srcdiags_t *diags);
void rp_ir_free(rp_ir_t *ir);

// The first dir whose ID is `id`, or NULL.
[[nodiscard]] const rp_ir_dir_t *rp_ir_find_dir(const rp_ir_t *ir, const char *id);

// RFC-0031 B9: a shared Win32 assembly (assembly = "win32" without assembly-for) goes into WinSxS
// with a signed catalog of its manifest, which the build makes with its --key. rp_ir_add_sxs_catalog
// adds it (`cat` is taken over) as <manifest name minus .manifest>.cat beside the manifest, in the
// DLL's component, after the IR is built and before it is lowered.
bool rp_ir_sxs_shared(const rp_ir_file_t *f);
[[nodiscard]] proven_err_t rp_ir_add_sxs_catalog(rp_ir_t *ir, size_t dll, uint8_t *cat, size_t len);

// The package's name as a registry key part (its letters and digits): a [menu.*] verb's key is
// "<that>.<menu ID>".
void rp_menu_token(const rp_ir_t *ir, char out[48]);

// A stable text dump of the model for golden tests.
[[nodiscard]] proven_err_t rp_ir_dump(const rp_ir_t *ir, proven_allocator_t alloc, uint8_t **out, size_t *len);

#endif // RUBRAPACK_IR_H
