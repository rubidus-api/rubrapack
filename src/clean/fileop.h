// src/clean/fileop.h - deleting what a path names, by a handle, shared by the helper DLL (src/ca: the
// wipe at removal) and the cleanup program (src/clean). Included by both.
//
// A check by name and a deletion by name leave a gap: a folder above the file, if someone else may
// change it (a user-made folder below ProgramData), can be put elsewhere by a junction between the
// two, and a program running as SYSTEM then deletes a file of another place. So an entry is opened
// once, as itself (not what a link at its end points at), its handle must say it is where its name
// says, and the deletion goes through that handle.

#ifndef RUBRAPACK_FILEOP_H
#define RUBRAPACK_FILEOP_H

#include <stdbool.h>
#include <string.h>
#include <windows.h>

// Opens `path` as itself (not what a link at its end points at) with `access` - DELETE to delete
// it, FILE_READ_ATTRIBUTES only to look at it (that opens a file another program holds without
// delete sharing too) - and gives where the handle says it is (`real`, `cap` characters) and its
// attributes. INVALID_HANDLE_VALUE otherwise, with the last error of the opening.
[[maybe_unused]] static HANDLE fileop_open_as(const wchar_t *path, DWORD access, wchar_t *real, DWORD cap, DWORD *attr) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE f = CreateFileW(path, access | SYNCHRONIZE, share, NULL, OPEN_EXISTING, flags, NULL);
    if (f == INVALID_HANDLE_VALUE) return f;
    FILE_ATTRIBUTE_TAG_INFO ti;
    DWORD n = GetFinalPathNameByHandleW(f, real, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!GetFileInformationByHandleEx(f, FileAttributeTagInfo, &ti, sizeof ti) || n == 0 || n >= cap) {
        CloseHandle(f);
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
        return INVALID_HANDLE_VALUE;
    }
    *attr = ti.FileAttributes;
    return f;
}

// Opens `path` ("\\?\C:\...") as itself, to delete it, and checks by the handle that it is where
// `path` says (`real` gets where it is): a folder above it put elsewhere by a link gives another
// place. INVALID_HANDLE_VALUE otherwise (the last error ERROR_CANT_RESOLVE_FILENAME then).
[[maybe_unused]] static HANDLE fileop_open(const wchar_t *path, wchar_t *real, DWORD cap, DWORD *attr) {
    HANDLE f = fileop_open_as(path, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, real, cap, attr);
    if (f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_CANT_RESOLVE_FILENAME)
        f = fileop_open_as(path, DELETE | FILE_READ_ATTRIBUTES, real, cap, attr);
    if (f != INVALID_HANDLE_VALUE && CompareStringOrdinal(real, -1, path, -1, TRUE) != CSTR_EQUAL) {
        CloseHandle(f);
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

// Deletes what `f` (fileop_open) is: a file, or a folder that is empty.
[[maybe_unused]] static bool fileop_delete(HANDLE f, DWORD attr) {
    if (attr & FILE_ATTRIBUTE_READONLY) {
        FILE_BASIC_INFO bi;
        memset(&bi, 0, sizeof bi);
        bi.FileAttributes = (attr & ~(DWORD)FILE_ATTRIBUTE_READONLY) ? (attr & ~(DWORD)FILE_ATTRIBUTE_READONLY) : FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(f, FileBasicInfo, &bi, sizeof bi);
    }
    // The name goes at once even while another program has the file open (Windows 10 1709 and
    // later: class 21, delete | POSIX semantics | ignore read-only); the older way otherwise.
    struct { DWORD flags; } ex = { 0x1 | 0x2 | 0x10 };
    if (SetFileInformationByHandle(f, (FILE_INFO_BY_HANDLE_CLASS)21, &ex, sizeof ex)) return true;
    FILE_DISPOSITION_INFO di = { TRUE };
    return SetFileInformationByHandle(f, FileDispositionInfo, &di, sizeof di) != 0;
}

#endif
