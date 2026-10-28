// Offline test of the save files' rules (src/savefiles_rules.h):
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_savefiles.cpp -o tests/build/test_savefiles.exe
//   wine tests/build/test_savefiles.exe ['Z:\...\re4.exe']
//
// The time stamp, the moments the list's frames are seen at (src/stamp.h), the
// getters' shapes and the request hash - and, given
// re4.exe, the getters' code the mod reads SaveFileDetail's layout out of, and
// the save list's own wait for the engine's table (the refresh the mod asks
// for) and its DialogWait (where a copy holds it still), at build 22377325's
// method RVAs (the database's code is only linked at run time;
// tools/tdb_dump.py reads the link records).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/savefiles_rules.h"
#include "../src/stamp.h"

namespace {

using namespace re4cc::savefiles;

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

// re4.exe's bytes at an RVA (from the section holding it).
struct Image {
  std::vector<uint8_t> file;
  struct Sec {
    uint32_t va, rsize, raw;
  };
  std::vector<Sec> secs;
  uint32_t u32(size_t at) const {
    return at + 4 <= file.size() ? file[at] | file[at + 1] << 8 | file[at + 2] << 16 | static_cast<uint32_t>(file[at + 3]) << 24 : 0;
  }
  uint16_t u16(size_t at) const { return at + 2 <= file.size() ? static_cast<uint16_t>(file[at] | file[at + 1] << 8) : 0; }
  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    file.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!ok || file.size() < 0x400) return false;
    const uint32_t pe = u32(0x3C);
    const uint16_t n = u16(pe + 6), opt = u16(pe + 20);
    for (uint16_t i = 0; i < n; ++i) {
      const size_t o = pe + 24 + opt + 40 * static_cast<size_t>(i);
      secs.push_back({u32(o + 12), u32(o + 16), u32(o + 20)});
    }
    return true;
  }
  bool read(uint32_t rva, uint8_t* out, size_t n) const {
    for (const auto& s : secs)
      if (rva >= s.va && rva + n <= static_cast<uint64_t>(s.va) + s.rsize) {
        std::memcpy(out, &file[s.raw + (rva - s.va)], n);
        return true;
      }
    return false;
  }
};

}  // namespace

int main(int argc, char** argv) {
  char what[256];

  // --- the time stamp -----------------------------------------------------------------------------------------------
  // 2026-09-19 12:00:00 UTC: Unix 1789819200; .NET ticks (Utc) = (unix + 62135596800) * 10^7.
  const long long unix_t = 1789819200LL;
  const long long ticks = (unix_t + 62135596800LL) * 10000000LL;
  std::snprintf(what, sizeof(what), "ticks %lld are Unix %lld (got %lld)", ticks, unix_t, rules::unix_seconds(ticks));
  check(rules::unix_seconds(ticks) == unix_t, what);
  check(rules::unix_seconds(0) == -1 && rules::unix_seconds(-5) == -1, "no time stamp: -1");
  check(rules::unix_seconds(unix_t) == -1, "Unix seconds taken for ticks: no plausible date, -1");

  // --- the moments (src/stamp.h) ------------------------------------------------------------------------------------
  // The list's frame notes a moment (GetTickCount() | 1), and the tick and the panel age it on their own clock reads:
  // one read in the moment's millisecond is 0 old, even or odd. (Aged on the bare clock, an even millisecond's
  // moment was a millisecond ahead - 0xFFFFFFFF old - and the list read as closed: the save files window flashed.)
  {
    namespace stamp = re4cc::stamp;
    check(stamp::age(1000 | 1, 1000) == 0 && stamp::within(1000 | 1, 400, 1000),
          "a moment of an even millisecond, read in it: 0 old, recent");
    check(stamp::age(1001, 1001) == 0 && stamp::within(1001, 400, 1001), "a moment of an odd millisecond, read in it: 0 old, recent");
    check(stamp::within(1001, 400, 1016) && stamp::within(1001, 400, 1398) && !stamp::within(1001, 400, 1402),
          "recent for 400 ms, not after");
    check(stamp::age(0xFFFFFFF1u, 0x10) == 0x20 && stamp::within(0xFFFFFFF1u, 400, 0x10), "across the clock's wrap: 32 ms old");
    check(!stamp::within(0, 400, 0) && !stamp::within(0, 400, 1000), "no moment: never recent");
    // The clock itself, for a quarter of a second: every moment is recent the instant it is taken. The bare clock's
    // count is only reported (it needs an even millisecond to come up).
    int n = 0, missed = 0, bare_missed = 0;
    for (const DWORD start = GetTickCount(); GetTickCount() - start < 250; ++n) {
      const DWORD m = stamp::now();
      if (!stamp::within(m, 400)) ++missed;
      if (GetTickCount() - m >= 400) ++bare_missed;
    }
    std::snprintf(what, sizeof(what), "%d moments, each recent as it is taken (missed %d; the bare clock missed %d)", n,
                  missed, bare_missed);
    check(n > 0 && missed == 0, what);
  }

  // --- the getters' code --------------------------------------------------------------------------------------------
  {
    const uint8_t slot[] = {0x8B, 0x42, 0x10, 0xC3};
    const uint8_t title[] = {0x48, 0x8B, 0x42, 0x18, 0xC3};
    const uint8_t far32[] = {0x48, 0x8B, 0x82, 0x00, 0x01, 0x00, 0x00, 0xC3};
    const uint8_t other[] = {0x48, 0x8B, 0x41, 0x18, 0xC3};  // [rcx+0x18]: not the object's
    const uint8_t invalid[] = {0xB8, 0x00, 0x00, 0x00, 0x80, 0xC3};
    int32_t c = 0;
    check(rules::getter_offset(slot, sizeof(slot), false) == 0x10, "mov eax,[rdx+0x10]; ret: 0x10");
    check(rules::getter_offset(slot, sizeof(slot), true) == -1, "a 32-bit load is not a 64-bit getter");
    check(rules::getter_offset(title, sizeof(title), true) == 0x18, "mov rax,[rdx+0x18]; ret: 0x18");
    check(rules::getter_offset(title, sizeof(title), false) == -1, "a 64-bit load is not a 32-bit getter");
    check(rules::getter_offset(far32, sizeof(far32), true) == 0x100, "mov rax,[rdx+0x100]; ret: 0x100");
    check(rules::getter_offset(other, sizeof(other), true) == -1, "mov rax,[rcx+0x18]: not a getter of the object");
    check(rules::getter_constant(invalid, sizeof(invalid), &c) && c == INT32_MIN, "mov eax,0x80000000; ret: INT32_MIN");
    check(!rules::getter_constant(slot, sizeof(slot), &c), "a load is not a constant");
  }

  if (argc > 1) {
    Image img;
    if (!img.load(argv[1])) {
      std::printf("cannot read %s\n", argv[1]);
      return 2;
    }
    // Build 22377325's method RVAs.
    struct Getter {
      const char* name;
      uint32_t rva;
      bool wide;
      int32_t want;
    };
    const Getter getters[] = {
        {"SaveFileDetail.get_Slot [55303]", 0xAEF70, false, 0x10},
        {"SaveFileDetail.get_LastUpdateTimeStamp [55304]", 0xB2EF0, true, 0x30},
    };
    for (const Getter& g : getters) {
      uint8_t b[10] = {};
      const int32_t got = img.read(g.rva, b, sizeof(b)) ? rules::getter_offset(b, sizeof(b), g.wide) : -2;
      std::snprintf(what, sizeof(what), "%s (exe+0x%X) reads +0x%X (got %d)", g.name, g.rva, g.want, got);
      check(got == g.want, what);
    }
    uint8_t b[32] = {};
    int32_t c = 0;
    check(img.read(0x531AF0, b, 6) && rules::getter_constant(b, 6, &c) && c == INT32_MIN,
          "SaveFileDetail.get_InvalidSlot [55309] (exe+0x531AF0) is 0x80000000");
    // SaveLoadMenuGuiBehavior.lateUpdateOnActive [55584] (exe+0x15C18B0): with <CurrSubPhase> (+0x1A4 from the
    // field pointer, +0x1F4 in the behaviour) at WaitUpdateTbl (1), it waits for the engine's table and the save
    // manager, then sets it back to None (0) and calls its own setup() [55621] - the refresh the mod asks for.
    auto rel32 = [&img](uint32_t at) -> uint32_t {
      uint8_t x[5] = {};
      int32_t d = 0;
      if (!img.read(at, x, sizeof(x)) || (x[0] != 0xE8 && x[0] != 0xE9)) return 0;
      std::memcpy(&d, x + 1, 4);
      return at + 5 + static_cast<uint32_t>(d);
    };
    const uint8_t wait[] = {0x83, 0xBE, 0xF4, 0x01, 0x00, 0x00, 0x01};      // cmp dword [rsi+0x1F4],1
    const uint8_t back[] = {0x44, 0x89, 0xA6, 0xF4, 0x01, 0x00, 0x00};      // mov [rsi+0x1F4],r12d (0)
    check(img.read(0x15C18FF, b, sizeof(wait)) && std::memcmp(b, wait, sizeof(wait)) == 0,
          "lateUpdateOnActive checks <CurrSubPhase> == WaitUpdateTbl first");
    check(img.read(0x15C2D12, b, sizeof(back)) && std::memcmp(b, back, sizeof(back)) == 0 &&
              rel32(0x15C2D21) == 0x1611550,
          "and once the table can be read, sets it back to None and calls setup() (exe+0x1611550)");
    // A copy holds the list still in <CurrPhase> (+0x1EC) DialogWait (5): lateUpdateOnActive's switch on it
    // (jump table exe+0x15C2D34) goes straight to its return, and checkEnableClose [55585] (exe+0x15725E0) lets
    // the list close only in Idle (0) or Finalize (15).
    const uint8_t epilogue[] = {0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00, 0x41, 0x5E, 0x41, 0x5C, 0x5E, 0x5D, 0xC3};
    uint8_t e[4] = {};
    uint32_t dialog_wait = 0;
    if (img.read(0x15C2D34 + 5 * 4, e, sizeof(e))) std::memcpy(&dialog_wait, e, sizeof(e));
    bool no_call = dialog_wait == 0x15C29C5 && img.read(0x15C29C5, b, 16);
    for (int i = 0; no_call && i < 16; ++i) no_call = b[i] != 0xE8;
    check(no_call && img.read(0x15C29E5, b, sizeof(epilogue)) && std::memcmp(b, epilogue, sizeof(epilogue)) == 0,
          "lateUpdateOnActive does nothing in DialogWait (its case is the function's return)");
    const uint8_t close[] = {0x8B, 0x82, 0xEC, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x05, 0x83,
                             0xF8, 0x0F, 0x75, 0x03, 0xB0, 0x01, 0xC3, 0x32, 0xC0, 0xC3};
    check(img.read(0x15725FE, b, sizeof(close)) && std::memcmp(b, close, sizeof(close)) == 0,
          "checkEnableClose: true only in Idle or Finalize - a list held in DialogWait cannot close");
  }

  std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "all passed", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
