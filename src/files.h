#pragma once

#include <windows.h>

// The files the mod keeps beside its DLL (the ini, the item boxes) are written
// whole into a temporary file and moved over the old one in one step.
namespace re4cc::files {

// MoveFileExA(from, to, flags), tried again a few times 100 ms apart while the
// file is only held for a moment: on Windows a virus scanner or the search
// indexer looking at the new file (or the old one) makes the replace fail with
// a sharing violation or "access denied". False with the last error in *error.
inline bool replace(const char* from, const char* to, DWORD flags, DWORD* error = nullptr) {
  for (int tries = 0;; ++tries) {
    if (MoveFileExA(from, to, flags)) return true;
    const DWORD err = GetLastError();
    if (tries >= 4 || (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)) {
      if (error) *error = err;
      SetLastError(err);
      return false;
    }
    Sleep(100);
  }
}

// DeleteFileA(path), tried again as replace() is: true once the file is gone (or
// was never there). False with the last error in *error.
inline bool remove(const char* path, DWORD* error = nullptr) {
  for (int tries = 0;; ++tries) {
    if (DeleteFileA(path)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
    if (tries >= 4 || (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)) {
      if (error) *error = err;
      SetLastError(err);
      return false;
    }
    Sleep(100);
  }
}

}  // namespace re4cc::files
