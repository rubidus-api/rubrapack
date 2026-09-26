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
// RP2013 component bitness does not match the summary's platform
// RP2014 a component's key path is not one of its own files
// RP2015 an upgrade property is not public or not in SecureCustomProperties

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/msi.h"
#include "rubrapack/srcdiag.h"

// Adds one diagnostic per finding (position 0:0: the finding is about the package, not a source
// line). Returns PROVEN_ERR_INVALID_STATE when any error was found, PROVEN_ERR_NOMEM, or PROVEN_OK.
[[nodiscard]] proven_err_t rp_msi_lint(proven_allocator_t alloc, const rp_msi_wdb_t *db, rp_srcdiags_t *diags);

#endif // RUBRAPACK_LINT_H
