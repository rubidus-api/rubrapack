#ifndef RUBRAPACK_LINT_H
#define RUBRAPACK_LINT_H

// include/rubrapack/lint.h - our own checks over a finished MSI table set (RFC-0001 14.1, P2
// "basic lint"). `build` runs them on every package before it is written; the rules are ours,
// written from the meaning of the Windows Installer tables, not a copy of ICE.
//
// RP2001 value kind does not match the column (string/integer/stream)
// RP2002 null in a column that is not nullable
// RP2003 string longer than the column width (counted in UTF-16 units)
// RP2004 integer outside the column's range (i2: -32767..32767; i4: not INT32_MIN)
// RP2005 string is not valid UTF-8
// RP2006 duplicate primary key
// RP2007 reference to a row that does not exist (foreign key, sequence action)
// RP2008 not a GUID in registry format ({XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}, upper case)
// RP2009 the same ComponentId on two components
// RP2010 a required property is missing
// RP2011 file sequence numbers and media do not agree
// RP2012 standard actions out of order
// RP2013 a 64-bit component in a 32-bit (Intel) package
// RP2014 a component's key path is not one of its own files
// RP2015 an upgrade property is not public or not in SecureCustomProperties
// RP2016 a rollback twin (`<Action>Rollback`) is sequenced after its forward action
// RP2017 an asynchronous custom action is also a rollback action
// RP2018 a custom action that runs a file names no File row
// RP2019 a deferred (script) custom action between InstallInitialize and RemoveExistingProducts
//
// Dialogs (RFC-0006 1; each seen to stop an installation on Windows 11 in P4):
// RP2100 (note) the database is not in code page 65001, so its text is not checked as UTF-8
// RP2101 a dialog's tab order (Control_Next) is not one cycle from Control_First (errors 2809, 2810)
// RP2102 a dialog's Control_First, Control_Default or Control_Cancel is not one of its controls
// RP2103 a FilesInUse dialog without a ListBox table (error 2205, the dialog is skipped)
// RP2104 the ErrorDialog lacks the ErrorText (Text) or ErrorIcon (Icon) control (error 2835)

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/msi.h"
#include "rubrapack/srcdiag.h"

// Adds one diagnostic per finding (position 0:0: the finding is about the package, not a source
// line). Returns PROVEN_ERR_INVALID_STATE when any error was found, PROVEN_ERR_NOMEM, or PROVEN_OK.
[[nodiscard]] proven_err_t rp_msi_lint(proven_allocator_t alloc, const rp_msi_wdb_t *db, rp_srcdiags_t *diags);

// `lint <file.msi>` (RFC-0006 L1): for a package made by any tool (`foreign`), only what breaks an
// installation is an error - the column rules RP2001, RP2002, RP2004, RP2005 (in 65001), RP2006,
// RP2007 and the dialog rules RP2101-RP2104; the rest are warnings unless `strict`.
typedef struct {
    bool foreign, strict;
} rp_lint_opts_t;

[[nodiscard]] proven_err_t rp_msi_lint_opts(proven_allocator_t alloc, const rp_msi_wdb_t *db, const rp_lint_opts_t *opts,
                                            rp_srcdiags_t *diags);

#endif // RUBRAPACK_LINT_H
