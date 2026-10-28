// Offline test of the challenges window's rules (src/challenges_rules.h) and names
// (src/challenges_text.h):
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_challenges.cpp -o tests/build/test_challenges.exe
//   wine tests/build/test_challenges.exe ['Z:\...\re4.exe']
//
// The CP's sum, a counted challenge's count and the things it counts, the names'
// table - and, given re4.exe, the game's own code the rules follow, at build
// 22377325's RVAs: unlockRecord's clamp of the CP, getProgressNum's challenges
// counted in the game save, and the shop's bonuses the game gives outside the
// challenges (the Mercenaries', a new game's, the requests').
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "../src/challenges_rules.h"
#include "../src/challenges_text.h"

namespace {

using namespace re4cc::challenges;
namespace text = re4cc::challenges_text;

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
  // The bytes at rva are `want` (a code shape the rules copy).
  bool is(uint32_t rva, std::initializer_list<uint8_t> want) const {
    std::vector<uint8_t> b(want.size());
    return read(rva, b.data(), b.size()) && std::memcmp(b.data(), want.begin(), b.size()) == 0;
  }
};

}  // namespace

int main(int argc, char** argv) {
  char what[256];

  // --- the CP -----------------------------------------------------------------------------------------------------
  check(rules::points_on(1000, 300) == 1300 && rules::points_off(1300, 300) == 1000, "a challenge's CP added, taken back");
  check(rules::points_off(100, 300) == 0, "taking back more than there is leaves 0");
  check(rules::points_on(rules::kMaxPoints - 10, 300) == rules::kMaxPoints, "the sum stops at the game's most");
  check(rules::points_on(0x7FFFFF00, 0x7FFFFF00) == rules::kMaxPoints, "no overflow");

  // --- a counted challenge ----------------------------------------------------------------------------------------
  check(rules::count_on(3, 50) == 50 && rules::count_on(70, 50) == 70, "on: the count at least its goal");
  check(rules::count_off(50, 50) == 49 && rules::count_off(70, 50) == 49 && rules::count_off(20, 50) == 20,
        "off: the count just short of its goal (one already short is left)");
  check(rules::count_on(0, 0) == 0 && rules::count_off(5, 0) == 5, "no goal: nothing changes");
  check(rules::bits_set(0) == 0 && rules::bits_set(0xFFFFFFFFu) == 32 && rules::bits_set(0x1Fu) == 5, "bits counted");
  check(rules::bits_off(0x1Fu, 4) == 0x0Fu, "off: the highest-numbered thing counted is forgotten first");
  check(rules::bits_off(0x80000001u, 1) == 1u && rules::bits_off(0x7u, 0) == 0u, "down to none, bit 31 too");
  check(rules::bits_off(0x5u, 4) == 0x5u && rules::bits_off(0x5u, -1) == 0u, "fewer than kept: left as they are");
  {
    // Gun Fanatic: 29 weapons counted once each, goal 29 -> 28 counted, 28 bits.
    const uint32_t all = (1u << 29) - 1;
    const int now = rules::count_off(29, 29);
    const uint32_t b = rules::bits_off(all, now);
    std::snprintf(what, sizeof(what), "a weapon forgotten: count %d, %d bits, the highest gone", now, rules::bits_set(b));
    check(now == 28 && rules::bits_set(b) == 28 && !(b & (1u << 28)), what);
  }
  {
    std::set<int> in_save;
    for (int id = 0; id < 300; ++id)
      if (rules::counted_in_game_save(id)) in_save.insert(id);
    const std::set<int> want = {77, 78, 79, 95, 96, 97, 103, 247, 248, 249};
    check(in_save == want, "the challenges counted in the game save: 077-079, 095-097, 103, 247-249");
  }

  // --- the names ----------------------------------------------------------------------------------------------------
  {
    std::set<int> ids;
    bool named = true, braces = true, hidden_ok = true;
    int hidden = 0;
    for (const auto& c : text::kChallenge) {
      ids.insert(c.id);
      named = named && c.name && c.name[0] && c.condition && c.condition[0];
      const char* at = std::strstr(c.condition, "{0}");
      braces = braces && (!at || !std::strstr(at + 3, "{0}")) && !std::strchr(c.condition, '<');
      hidden += c.hidden;
      hidden_ok = hidden_ok && (!c.hidden || std::strncmp(c.condition, "Complete ", 9) == 0);
    }
    std::snprintf(what, sizeof(what), "%d challenges named, each once (%d unique ids)", text::kChallenges,
                  static_cast<int>(ids.size()));
    check(text::kChallenges >= 140 && static_cast<int>(ids.size()) == text::kChallenges && named, what);
    check(braces, "conditions: one {0} at most, no markup left");
    std::snprintf(what, sizeof(what), "%d hidden challenges, named by what they unlock, a condition from the shop", hidden);
    check(hidden == 2 && hidden_ok, what);
    check(ids.count(0) && ids.count(1) && ids.count(2) && ids.count(201) && ids.count(249) && !ids.count(200),
          "the main story's and Separate Ways' (the rejected 200 left out)");
  }
  {
    std::set<int> ids;
    bool ok = true;
    for (const auto& b : text::kBonus) {
      ids.insert(b.id);
      ok = ok && b.name && b.name[0] && b.who && b.kind >= text::kMode && b.kind <= text::kFilter &&
           !std::strchr(b.name, '<');
    }
    std::snprintf(what, sizeof(what), "%d shop bonuses named, each once", text::kBonuses);
    check(ok && static_cast<int>(ids.size()) == text::kBonuses && text::kBonuses > 200, what);
    check(ids.count(0) && ids.count(1), "the two game modes (Professional) named");
  }

  // --- the game's code, build 22377325 --------------------------------------------------------------------------------
  if (argc < 2) {
    std::printf("(no re4.exe given: the game's code not checked)\n");
  } else {
    Image img;
    if (!img.load(argv[1])) {
      check(false, "re4.exe read");
    } else {
      // GameRecordManager.unlockRecord: `mov ebp,0x5F5E100; cmp eax,ebp; cmovl ebp,eax` - the CP's most.
      check(img.is(0x1D9AFE8, {0xBD, 0x00, 0xE1, 0xF5, 0x05, 0x3B, 0xC5, 0x0F, 0x4C, 0xE8}),
            "unlockRecord keeps the CP at 100,000,000 at most (kMaxPoints)");
      // getProgressNum: `lea eax,[r8-0x4D]; ... cmp eax,0x1A; ja; mov ecx,0x41C0007; bt ecx,eax; jb <game save>`,
      // then `lea eax,[r8-0xF7]; cmp eax,1; jbe <game save>`, `cmp esi,0xF9; je <game save>`.
      const bool shape = img.is(0x1D0D69A, {0x41, 0x8D, 0x40, 0xB3}) && img.is(0x1D0D6A7, {0x83, 0xF8, 0x1A, 0x77}) &&
                         img.is(0x1D0D6AC, {0xB9, 0x07, 0x00, 0x1C, 0x04, 0x0F, 0xA3, 0xC1, 0x0F, 0x82}) &&
                         img.is(0x1D0D6BA, {0x41, 0x8D, 0x80, 0x09, 0xFF, 0xFF, 0xFF, 0x83, 0xF8, 0x01, 0x0F, 0x86}) &&
                         img.is(0x1D0D6CA, {0x81, 0xFE, 0xF9, 0x00, 0x00, 0x00, 0x74});
      bool same = shape;
      for (int id = 0; id < 512 && same; ++id) {
        const unsigned d = static_cast<unsigned>(id - 0x4D);
        const bool game = (d <= 0x1A && ((0x41C0007u >> d) & 1u)) || static_cast<unsigned>(id - 0xF7) <= 1u || id == 0xF9;
        same = game == rules::counted_in_game_save(id);
      }
      check(same, "getProgressNum reads exactly those ten from the game save");
      // AdditionalBonusManager.canUnlockBonus while the Mercenaries runs: 7, 0x1A8, 0x1AA..0x1AE.
      const bool mercs = img.is(0x24B6062, {0x83, 0xFB, 0x07, 0x74}) &&
                         img.is(0x24B6067, {0x81, 0xFB, 0xA8, 0x01, 0x00, 0x00, 0x74}) &&
                         img.is(0x24B606F, {0x8D, 0x83, 0x56, 0xFE, 0xFF, 0xFF, 0x83, 0xF8, 0x04, 0x76});
      // ExShopData.get_isNewGameUnlock: 0x80, 0x99 (the main story), 0x196 (Separate Ways).
      const bool new_game = img.is(0x178F597, {0x3D, 0x80, 0x00, 0x00, 0x00, 0x74}) &&
                            img.is(0x178F59E, {0x3D, 0x99, 0x00, 0x00, 0x00, 0x74}) &&
                            img.is(0x178F5A5, {0x3D, 0x96, 0x01, 0x00, 0x00, 0x75});
      // GameRecordManager.checkReleaseBonusToQuest: `mov r8d,0xD1 / 0xD3 / 0xD4; jmp setUnlockBonus`.
      const bool quests = img.is(0x1CDF769, {0x41, 0xB8, 0xD1, 0x00, 0x00, 0x00, 0xE9}) &&
                          img.is(0x1CDF77D, {0x41, 0xB8, 0xD3, 0x00, 0x00, 0x00, 0xE9}) &&
                          img.is(0x1CDF791, {0x41, 0xB8, 0xD4, 0x00, 0x00, 0x00, 0xE9});
      bool given = mercs && new_game && quests;
      for (int b = 0; b < 512 && given; ++b) {
        const bool game = b == 7 || b == 0x1A8 || (b >= 0x1AA && b <= 0x1AE) || b == 0x80 || b == 0x99 || b == 0x196 ||
                          b == 0xD1 || b == 0xD3 || b == 0xD4;
        given = game == rules::given_by_code(b);
      }
      std::snprintf(what, sizeof(what), "the bonuses given outside the challenges (Mercenaries %d, new game %d, requests %d)",
                    mercs, new_game, quests);
      check(given, what);
    }
  }

  std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "all passed", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
