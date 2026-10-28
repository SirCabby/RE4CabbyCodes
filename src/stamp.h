#pragma once

#include <windows.h>

// Moments one thread notes for another (or for itself, later): GetTickCount()
// with bit 0 set, so that none reads as 0 - "never". Their age is taken on a
// clock with bit 0 set too. GetTickCount() holds a value for up to 16 ms (on
// Windows and under Wine alike), and a moment noted while it held an even one
// was a millisecond ahead of it: an age of -1, which unsigned is 49 days. The
// save list was taken for closed whenever it was read so, and its window
// flashed.
//
// A moment another thread writes is loaded before the clock is read (these read
// the clock after their arguments), so it is never from a later millisecond.
namespace re4cc::stamp {

inline DWORD now() { return GetTickCount() | 1; }

// Milliseconds from the moment `at` to `clock`, a GetTickCount() read after it
// (to within a millisecond).
inline DWORD age(DWORD at, DWORD clock) { return (clock | 1) - at; }
inline DWORD age(DWORD at) { return age(at, GetTickCount()); }

// `at` is a moment less than `ms` ago (0, none: never).
inline bool within(DWORD at, DWORD ms, DWORD clock) { return at && age(at, clock) < ms; }
inline bool within(DWORD at, DWORD ms) { return within(at, ms, GetTickCount()); }

}  // namespace re4cc::stamp
