// src/msi/validation.c - the _Validation table of a package (ICE03; RFC-0016 1).
//
// Windows Installer does not read _Validation when it installs; validation tools do: each row says
// what a column may hold - null or not, a range, the table a value refers to, a category such as
// Identifier or Formatted, a set of values. The rules below follow the column descriptions of the
// standard tables on Microsoft Learn ("Database Tables"). A table or column rubrapack writes that
// has no rule here stops the build (a test builds every table), so the list cannot fall behind.

#include "rubrapack/msi.h"
#include "rubrapack/mem.h"

#include <limits.h>
#include <string.h>

enum { NONE = INT_MIN };        // no MinValue / MaxValue

typedef struct {
    const char *table, *column;
    int32_t     min, max;
    const char *keytable;
    int16_t     keycol;
    const char *category, *set;
} rule_t;

#define R(t, c, cat)                     { t, c, NONE, NONE, NULL, 0, cat, NULL }
#define RK(t, c, kt, kc)                 { t, c, NONE, NONE, kt, kc, "Identifier", NULL }
#define RN(t, c, lo, hi)                 { t, c, lo, hi, NULL, 0, NULL, NULL }
#define RS(t, c, cat, set)               { t, c, NONE, NONE, NULL, 0, cat, set }
#define MAXI 2147483647

static const rule_t rules[] = {
    R("_Validation", "Table", "Identifier"), R("_Validation", "Column", "Identifier"),
    RS("_Validation", "Nullable", "Text", "Y;N"), RN("_Validation", "MinValue", -2147483647, MAXI),
    RN("_Validation", "MaxValue", -2147483647, MAXI), R("_Validation", "KeyTable", "Identifier"),
    RN("_Validation", "KeyColumn", 1, 32), R("_Validation", "Category", "Text"), R("_Validation", "Set", "Text"),
    R("_Validation", "Description", "Text"),

    R("Property", "Property", "Identifier"), R("Property", "Value", "Text"),
    R("Directory", "Directory", "Identifier"), RK("Directory", "Directory_Parent", "Directory", 1),
    R("Directory", "DefaultDir", "DefaultDir"),
    R("Component", "Component", "Identifier"), R("Component", "ComponentId", "Guid"),
    RK("Component", "Directory_", "Directory", 1), RN("Component", "Attributes", 0, 32767),
    R("Component", "Condition", "Condition"), RK("Component", "KeyPath", "File;Registry;ODBCDataSource", 1),
    R("Feature", "Feature", "Identifier"), RK("Feature", "Feature_Parent", "Feature", 1), R("Feature", "Title", "Text"),
    R("Feature", "Description", "Text"), RN("Feature", "Display", 0, 32767), RN("Feature", "Level", 0, 32767),
    { "Feature", "Directory_", NONE, NONE, "Directory", 1, "UpperCase", NULL }, RN("Feature", "Attributes", 0, 63),
    RK("FeatureComponents", "Feature_", "Feature", 1), RK("FeatureComponents", "Component_", "Component", 1),
    R("File", "File", "Identifier"), RK("File", "Component_", "Component", 1), R("File", "FileName", "Filename"),
    RN("File", "FileSize", 0, MAXI), { "File", "Version", NONE, NONE, "File", 1, "Version", NULL },
    R("File", "Language", "Language"), RN("File", "Attributes", 0, 32767), RN("File", "Sequence", 1, MAXI),
    RK("CreateFolder", "Directory_", "Directory", 1), RK("CreateFolder", "Component_", "Component", 1),
    RK("MsiFileHash", "File_", "File", 1), RN("MsiFileHash", "Options", 0, 0),
    { "MsiFileHash", "HashPart1", NONE, NONE, NULL, 0, NULL, NULL }, { "MsiFileHash", "HashPart2", NONE, NONE, NULL, 0, NULL, NULL },
    { "MsiFileHash", "HashPart3", NONE, NONE, NULL, 0, NULL, NULL }, { "MsiFileHash", "HashPart4", NONE, NONE, NULL, 0, NULL, NULL },
    RN("Media", "DiskId", 1, 32767), RN("Media", "LastSequence", 0, MAXI), R("Media", "DiskPrompt", "Text"),
    R("Media", "Cabinet", "Cabinet"), R("Media", "VolumeLabel", "Text"), R("Media", "Source", "Property"),
    R("Upgrade", "UpgradeCode", "Guid"), R("Upgrade", "VersionMin", "Text"), R("Upgrade", "VersionMax", "Text"),
    R("Upgrade", "Language", "Language"), RN("Upgrade", "Attributes", 0, MAXI), R("Upgrade", "Remove", "Formatted"),
    R("Upgrade", "ActionProperty", "UpperCase"),
    R("CustomAction", "Action", "Identifier"), RN("CustomAction", "Type", 1, 32767),
    R("CustomAction", "Source", "CustomSource"), R("CustomAction", "Target", "Formatted"),
    R("Registry", "Registry", "Identifier"), RN("Registry", "Root", -1, 3), R("Registry", "Key", "RegPath"),
    R("Registry", "Name", "Formatted"), R("Registry", "Value", "Formatted"), RK("Registry", "Component_", "Component", 1),
    R("RemoveRegistry", "RemoveRegistry", "Identifier"), RN("RemoveRegistry", "Root", -1, 3),
    R("RemoveRegistry", "Key", "RegPath"), R("RemoveRegistry", "Name", "Formatted"),
    RK("RemoveRegistry", "Component_", "Component", 1),
    R("Shortcut", "Shortcut", "Identifier"), RK("Shortcut", "Directory_", "Directory", 1), R("Shortcut", "Name", "Filename"),
    RK("Shortcut", "Component_", "Component", 1), R("Shortcut", "Target", "Shortcut"), R("Shortcut", "Arguments", "Formatted"),
    R("Shortcut", "Description", "Text"), RN("Shortcut", "Hotkey", 0, 32767), RK("Shortcut", "Icon_", "Icon", 1),
    RN("Shortcut", "IconIndex", -32767, 32767), { "Shortcut", "ShowCmd", NONE, NONE, NULL, 0, "Integer", "1;3;7" },
    R("Shortcut", "WkDir", "Identifier"),
    R("RemoveFile", "FileKey", "Identifier"), RK("RemoveFile", "Component_", "Component", 1),
    R("RemoveFile", "FileName", "WildCardFilename"), R("RemoveFile", "DirProperty", "Identifier"),
    { "RemoveFile", "InstallMode", NONE, NONE, NULL, 0, NULL, "1;2;3" },
    R("DuplicateFile", "FileKey", "Identifier"), RK("DuplicateFile", "Component_", "Component", 1),
    RK("DuplicateFile", "File_", "File", 1), R("DuplicateFile", "DestName", "Filename"),
    R("DuplicateFile", "DestFolder", "Identifier"),
    R("Environment", "Environment", "Identifier"), R("Environment", "Name", "Text"), R("Environment", "Value", "Formatted"),
    RK("Environment", "Component_", "Component", 1),
    R("IniFile", "IniFile", "Identifier"), R("IniFile", "FileName", "Filename"), R("IniFile", "DirProperty", "Identifier"),
    R("IniFile", "Section", "Formatted"), R("IniFile", "Key", "Formatted"), R("IniFile", "Value", "Formatted"),
    { "IniFile", "Action", NONE, NONE, NULL, 0, NULL, "0;1;3" }, RK("IniFile", "Component_", "Component", 1),
    R("RemoveIniFile", "RemoveIniFile", "Identifier"), R("RemoveIniFile", "FileName", "Filename"),
    R("RemoveIniFile", "DirProperty", "Identifier"), R("RemoveIniFile", "Section", "Formatted"),
    R("RemoveIniFile", "Key", "Formatted"), R("RemoveIniFile", "Value", "Formatted"),
    { "RemoveIniFile", "Action", NONE, NONE, NULL, 0, NULL, "2;4" }, RK("RemoveIniFile", "Component_", "Component", 1),
    R("LaunchCondition", "Condition", "Condition"), R("LaunchCondition", "Description", "Formatted"),
    R("AppSearch", "Property", "Identifier"), R("AppSearch", "Signature_", "Identifier"),
    R("RegLocator", "Signature_", "Identifier"), RN("RegLocator", "Root", 0, 3), R("RegLocator", "Key", "RegPath"),
    R("RegLocator", "Name", "Formatted"), RN("RegLocator", "Type", 0, 18),
    R("DrLocator", "Signature_", "Identifier"), R("DrLocator", "Parent", "Identifier"), R("DrLocator", "Path", "AnyPath"),
    RN("DrLocator", "Depth", 0, 32767),
    R("Signature", "Signature", "Identifier"), R("Signature", "FileName", "Filename"), R("Signature", "MinVersion", "Text"),
    R("Signature", "MaxVersion", "Text"), RN("Signature", "MinSize", 0, MAXI), RN("Signature", "MaxSize", 0, MAXI),
    RN("Signature", "MinDate", 0, MAXI), RN("Signature", "MaxDate", 0, MAXI), R("Signature", "Languages", "Language"),
    R("CompLocator", "Signature_", "Identifier"), R("CompLocator", "ComponentId", "Guid"), RN("CompLocator", "Type", 0, 1),
    R("ServiceInstall", "ServiceInstall", "Identifier"), R("ServiceInstall", "Name", "Formatted"),
    R("ServiceInstall", "DisplayName", "Formatted"), RN("ServiceInstall", "ServiceType", -2147483647, MAXI),
    RN("ServiceInstall", "StartType", 0, 4), RN("ServiceInstall", "ErrorControl", -2147483647, MAXI),
    R("ServiceInstall", "LoadOrderGroup", "Formatted"), R("ServiceInstall", "Dependencies", "Formatted"),
    R("ServiceInstall", "StartName", "Formatted"), R("ServiceInstall", "Password", "Formatted"),
    R("ServiceInstall", "Arguments", "Formatted"), RK("ServiceInstall", "Component_", "Component", 1),
    R("ServiceInstall", "Description", "Text"),
    R("ServiceControl", "ServiceControl", "Identifier"), R("ServiceControl", "Name", "Formatted"),
    RN("ServiceControl", "Event", 0, 187), R("ServiceControl", "Arguments", "Formatted"), RN("ServiceControl", "Wait", 0, 1),
    RK("ServiceControl", "Component_", "Component", 1),
    RK("Font", "File_", "File", 1), R("Font", "FontTitle", "Text"),
    R("MsiLockPermissionsEx", "MsiLockPermissionsEx", "Identifier"), R("MsiLockPermissionsEx", "LockObject", "Identifier"),
    RS("MsiLockPermissionsEx", "Table", "Text", "CreateFolder;File;Registry;ServiceInstall"),
    R("MsiLockPermissionsEx", "SDDLText", "FormattedSDDLText"), R("MsiLockPermissionsEx", "Condition", "Condition"),
    R("Binary", "Name", "Identifier"), R("Binary", "Data", "Binary"),
    R("Icon", "Name", "Identifier"), R("Icon", "Data", "Binary"),
    RK("Condition", "Feature_", "Feature", 1), RN("Condition", "Level", 0, 32767), R("Condition", "Condition", "Condition"),
    R("InstallExecuteSequence", "Action", "Identifier"), R("InstallExecuteSequence", "Condition", "Condition"),
    RN("InstallExecuteSequence", "Sequence", -4, 32767),
    R("InstallUISequence", "Action", "Identifier"), R("InstallUISequence", "Condition", "Condition"),
    RN("InstallUISequence", "Sequence", -4, 32767),
    R("AdminExecuteSequence", "Action", "Identifier"), R("AdminExecuteSequence", "Condition", "Condition"),
    RN("AdminExecuteSequence", "Sequence", -4, 32767),
    R("AdminUISequence", "Action", "Identifier"), R("AdminUISequence", "Condition", "Condition"),
    RN("AdminUISequence", "Sequence", -4, 32767),
    R("AdvtExecuteSequence", "Action", "Identifier"), R("AdvtExecuteSequence", "Condition", "Condition"),
    RN("AdvtExecuteSequence", "Sequence", -4, 32767),
    R("Dialog", "Dialog", "Identifier"), RN("Dialog", "HCentering", 0, 100), RN("Dialog", "VCentering", 0, 100),
    RN("Dialog", "Width", 0, 32767), RN("Dialog", "Height", 0, 32767), RN("Dialog", "Attributes", 0, MAXI),
    R("Dialog", "Title", "Formatted"), RK("Dialog", "Control_First", "Control", 2),
    RK("Dialog", "Control_Default", "Control", 2), RK("Dialog", "Control_Cancel", "Control", 2),
    RK("Control", "Dialog_", "Dialog", 1), R("Control", "Control", "Identifier"), R("Control", "Type", "Identifier"),
    RN("Control", "X", 0, 32767), RN("Control", "Y", 0, 32767), RN("Control", "Width", 0, 32767),
    RN("Control", "Height", 0, 32767), RN("Control", "Attributes", 0, MAXI), R("Control", "Property", "Identifier"),
    R("Control", "Text", "Formatted"), RK("Control", "Control_Next", "Control", 2), R("Control", "Help", "Text"),
    RK("ControlEvent", "Dialog_", "Dialog", 1), RK("ControlEvent", "Control_", "Control", 2),
    R("ControlEvent", "Event", "Formatted"), R("ControlEvent", "Argument", "Formatted"),
    R("ControlEvent", "Condition", "Condition"), RN("ControlEvent", "Ordering", 0, MAXI),
    RK("ControlCondition", "Dialog_", "Dialog", 1), RK("ControlCondition", "Control_", "Control", 2),
    RS("ControlCondition", "Action", "Text", "Default;Disable;Enable;Hide;Show"),
    R("ControlCondition", "Condition", "Condition"),
    RK("EventMapping", "Dialog_", "Dialog", 1), RK("EventMapping", "Control_", "Control", 2),
    R("EventMapping", "Event", "Identifier"), R("EventMapping", "Attribute", "Identifier"),
    R("TextStyle", "TextStyle", "Identifier"), R("TextStyle", "FaceName", "Text"), RN("TextStyle", "Size", 0, 32767),
    RN("TextStyle", "Color", 0, 16777215), RN("TextStyle", "StyleBits", 0, 15),
    R("UIText", "Key", "Identifier"), R("UIText", "Text", "Text"),
    R("RadioButton", "Property", "Identifier"), RN("RadioButton", "Order", 1, 32767),
    R("RadioButton", "Value", "Formatted"), RN("RadioButton", "X", 0, 32767), RN("RadioButton", "Y", 0, 32767),
    RN("RadioButton", "Width", 0, 32767), RN("RadioButton", "Height", 0, 32767), R("RadioButton", "Text", "Text"),
    R("RadioButton", "Help", "Text"),
    R("ListBox", "Property", "Identifier"), RN("ListBox", "Order", 1, 32767), R("ListBox", "Value", "Formatted"),
    R("ListBox", "Text", "Text"),
    R("ComboBox", "Property", "Identifier"), RN("ComboBox", "Order", 1, 32767), R("ComboBox", "Value", "Formatted"),
    R("ComboBox", "Text", "Text"),
};

static const rp_msi_wcolumn_t validation_cols[] = {
    { "Table", 0x2D20 }, { "Column", 0x2D20 }, { "Nullable", 0x0D04 }, { "MinValue", 0x1104 }, { "MaxValue", 0x1104 },
    { "KeyTable", 0x1DFF }, { "KeyColumn", 0x1502 }, { "Category", 0x1D20 }, { "Set", 0x1DFF }, { "Description", 0x1DFF },
};
enum { NCOL = sizeof validation_cols / sizeof validation_cols[0] };

static const rule_t *find(const char *table, const char *column) {
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; ++i) {
        if (strcmp(rules[i].table, table) == 0 && strcmp(rules[i].column, column) == 0) return &rules[i];
    }
    return NULL;
}

static rp_msi_cell_t str(const char *s) {
    return s ? (rp_msi_cell_t){ .kind = RP_MSI_STR, .bytes = (const uint8_t *)s, .len = strlen(s) } : (rp_msi_cell_t){ .kind = RP_MSI_NULL };
}

static rp_msi_cell_t num(int32_t v) {
    return v == NONE ? (rp_msi_cell_t){ .kind = RP_MSI_NULL } : (rp_msi_cell_t){ .kind = RP_MSI_INT, .i = v };
}

proven_err_t rp_msi_validation(proven_allocator_t alloc, const rp_msi_wtable_t *tables, size_t count,
                               rp_msi_wtable_t *out, const char **missing) {
    if ((tables == NULL && count) || out == NULL) return PROVEN_ERR_INVALID_ARG;
    *out = (rp_msi_wtable_t){ .name = "_Validation", .columns = validation_cols, .column_count = NCOL };
    if (missing) *missing = NULL;
    size_t rows = NCOL;
    for (size_t t = 0; t < count; ++t) rows += tables[t].column_count;
    rp_msi_cell_t *cells = rp_mem_alloc(alloc, rows * NCOL, sizeof *cells);
    if (cells == NULL) return PROVEN_ERR_NOMEM;
    size_t r = 0;
    for (size_t t = 0; t <= count; ++t) {
        const rp_msi_wtable_t *wt = t < count ? &tables[t] : out;
        for (size_t c = 0; c < wt->column_count; ++c) {
            const rp_msi_wcolumn_t *col = &wt->columns[c];
            const rule_t *rule = find(wt->name, col->name);
            if (rule == NULL) {
                rp_mem_free(alloc, cells);
                if (missing) *missing = wt->name;
                return PROVEN_ERR_NOT_FOUND;
            }
            rp_msi_cell_t *row = &cells[r++ * NCOL];
            row[0] = str(wt->name);
            row[1] = str(col->name);
            row[2] = str((col->type & 0x1000) ? "Y" : "N");
            row[3] = num(rule->min);
            row[4] = num(rule->max);
            row[5] = str(rule->keytable);
            row[6] = rule->keycol ? num(rule->keycol) : num(NONE);
            row[7] = str(rule->category);
            row[8] = str(rule->set);
            row[9] = str(NULL);
        }
    }
    out->cells = cells;
    out->row_count = r;
    return PROVEN_OK;
}
