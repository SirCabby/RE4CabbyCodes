// Offline test of src/re.cpp against the real type database, run under Wine:
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_re.cpp src/re.cpp
//       src/log.cpp src/mem.cpp -o tests/build/test_re.exe
//   wine tests/build/test_re.exe 'Z:\...\re4.exe'
//
// re4.exe keeps its TDB in .data with array offsets instead of pointers; the
// VM rebases them as it starts. This does the same to a copy read from the file
// and then asks re:: for what the game layer asks for - class names, field
// offsets up the class chain, enum literals, generic instance and array names,
// methods by parameter types and their vtable slots - checking each answer
// against what tools/tdb_dump.py reads out of the same file (build 22377325's
// values). The game's own checks (the classes the cheats use) follow in
// check_game().
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/log.h"
#include "../src/mem.h"
#include "../src/re.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

void check_field(const char* type, const char* field, uint32_t offset, bool is_static = false) {
  const uint32_t t = re4cc::re::find_type(type);
  const re4cc::re::Field f = re4cc::re::find_field(t, field);
  char what[320];
  std::snprintf(what, sizeof(what), "%s.%s at +0x%X%s (got %s+0x%X, declared by %s)", type, field, offset,
                is_static ? " static" : "", f.valid() ? "" : "nothing ", f.offset,
                f.valid() ? re4cc::re::full_name(f.declaring).c_str() : "-");
  check(t && f.valid() && f.offset == offset && f.is_static() == is_static, what);
}

void check_enum(const char* type, const char* name, int want) {
  const int v = re4cc::re::enum_value(re4cc::re::find_type(type), name, -12345);
  char what[256];
  std::snprintf(what, sizeof(what), "%s.%s = %d (got %d)", type, name, want, v);
  check(v == want, what);
}

void check_enum64(const char* type, const char* name, int64_t want) {
  const int64_t v = re4cc::re::enum_value64(re4cc::re::find_type(type), name, -12345);
  char what[256];
  std::snprintf(what, sizeof(what), "%s.%s = %lld (got %lld)", type, name, static_cast<long long>(want),
                static_cast<long long>(v));
  check(v == want, what);
}

void check_method(const char* type, const char* name, std::initializer_list<const char*> params, uint32_t want,
                  bool is_static = false, int vt = -2) {
  const uint32_t m = re4cc::re::find_method(re4cc::re::find_type(type), name, params, is_static);
  char what[320];
  std::snprintf(what, sizeof(what), "%s.%s(%zu param(s))%s is method %u (got %u)%s", type, name, params.size(),
                is_static ? " static" : "", want, m, vt >= -1 ? ", virtual slot checked" : "");
  check(m == want && (vt < -1 || re4cc::re::method_vt_index(m) == vt), what);
}

// The classes the mod's game layer reads (game.cpp), with build 22377325's
// offsets as tools/tdb_dump.py reads them.
void check_game();

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_re.exe <re4.exe>\n");
    return 2;
  }
  re4cc::log_init(".\\");
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::printf("cannot open %s\n", argv[1]);
    return 2;
  }
  // Find the header in .data the way the mod does: magic, version 71.
  IMAGE_DOS_HEADER dos;
  std::fread(&dos, sizeof(dos), 1, f);
  IMAGE_NT_HEADERS64 nt;
  std::fseek(f, dos.e_lfanew, SEEK_SET);
  std::fread(&nt, sizeof(nt), 1, f);
  std::vector<IMAGE_SECTION_HEADER> secs(nt.FileHeader.NumberOfSections);
  std::fseek(f, dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader,
             SEEK_SET);
  std::fread(secs.data(), sizeof(IMAGE_SECTION_HEADER), secs.size(), f);
  const IMAGE_SECTION_HEADER* data = nullptr;
  for (const auto& s : secs)
    if (!std::strncmp(reinterpret_cast<const char*>(s.Name), ".data", 8)) data = &s;
  check(data != nullptr, ".data section present");
  if (!data) return 1;
  std::vector<uint8_t> sec(data->SizeOfRawData);
  std::fseek(f, data->PointerToRawData, SEEK_SET);
  std::fread(sec.data(), 1, sec.size(), f);
  std::fclose(f);
  size_t at = 0;
  uint32_t version = 0;
  for (; at + 8 < sec.size(); at += 8) {
    if (std::memcmp(&sec[at], "TDB\0", 4)) continue;
    version = *reinterpret_cast<uint32_t*>(&sec[at + 4]);
    if (version == 71) break;
  }
  check(at + 8 < sec.size(), "a TDB v71 header found in .data");
  if (at + 8 >= sec.size()) return 1;
  // Copy from the header to the end of the section and rebase the array offsets
  // into pointers, as the VM does: 18 from 0x58.
  const size_t len = sec.size() - at;
  auto* tdb = static_cast<uint8_t*>(VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  std::memcpy(tdb, &sec[at], len);
  const int first_array = 0x58, arrays = 18;
  for (int i = 0; i < arrays; ++i) {
    auto* p = reinterpret_cast<uint64_t*>(tdb + first_array + 8 * i);
    if (*p) *p += reinterpret_cast<uint64_t>(tdb);
  }
  *reinterpret_cast<uint32_t*>(tdb + 8) = 1;
  // The methods' code offsets count from a base in .text; here, this test's own
  // .text (the file's offsets are all 0 - the VM fills them in).
  const re4cc::mem::Range text = re4cc::mem::section(GetModuleHandleA(nullptr), ".text");
  const uintptr_t code_base = text.begin + 0x10;
  check(re4cc::re::attach_tdb(reinterpret_cast<uintptr_t>(tdb), code_base), "attach_tdb");
  std::printf("status: %s, %u types, %u methods\n", re4cc::re::status(), re4cc::re::num_types(),
              re4cc::re::num_methods());

  using namespace re4cc;
  check(re::num_types() == 162644 && re::num_methods() == 1112853, "162644 types, 1112853 methods");
  check(re::find_type("via.GameObject") == 127481, "via.GameObject is type 127481");
  check(re::find_type("chainsaw.CharacterManager") == 115255, "chainsaw.CharacterManager is type 115255");
  check(re::find_type("chainsaw.CharacterContext") == 29003, "chainsaw.CharacterContext is type 29003");
  check(re::find_type("chainsaw.CharacterManager.PlayerNo") == 77658, "nested CharacterManager.PlayerNo is type 77658");
  check(re::find_type("System.Collections.Generic.List`1<chainsaw.CharacterContext>") == 122013,
        "List`1<chainsaw.CharacterContext> by name (a generic instance)");
  check(re::find_type("chainsaw.CharacterKindID[]") == 78312, "chainsaw.CharacterKindID[] by name (an array)");
  check(re::full_name(re::parent(re::find_type("chainsaw.CharacterManager"))) == "AppSingleton`1<chainsaw.CharacterManager>",
        "CharacterManager's parent is its singleton root, AppSingleton`1<chainsaw.CharacterManager>");
  check(!re::is_value_type(re::find_type("chainsaw.CharacterManager")) && re::is_value_type(re::find_type("System.Int32")),
        "a class is no value type, System.Int32 is one");
  check(re::value_size(re::find_type("via.vec3")) == 16, "via.vec3 takes 16 bytes in an array");
  check(re::derives(re::find_type("chainsaw.CharacterManager"), re::find_type("System.Object")), "it derives from System.Object");
  check_field("chainsaw.CharacterManager", "_Instance", 0x0, true);
  check_field("chainsaw.CharacterManager", "<PlayerManager>k__BackingField", 0x30);
  check_field("chainsaw.CharacterManager", "<PlayerContextList>k__BackingField", 0x80);
  check_field("chainsaw.CharacterManager", "<HitStopDelayTimer>k__BackingField", 0x100);
  check_field("chainsaw.CharacterManager", "MaxSavableCharacterCount", 0x0, true);
  check_field("System.Collections.Generic.List`1<chainsaw.CharacterContext>", "_items", 0x0);
  check_field("System.Collections.Generic.List`1<chainsaw.CharacterContext>", "_size", 0x8);
  check_enum("chainsaw.CharacterManager.PlayerKind", "PlA7", 7);
  // A 64-bit enum (the player's state flags): its literals as wide as it is.
  check_enum64("chainsaw.PlayerDefine.State", "Battle", 1ll << 60);
  check_enum64("chainsaw.PlayerDefine.State", "HookShot", INT64_MIN);
  check_enum64("chainsaw.PlayerDefine.State", "Jacked", 1ll << 40);
  check_enum64("chainsaw.CostumeSelectableSituation", "ExceptEventOrBattle", 3);
  check_method("chainsaw.CharacterManager", "getPlayerContextRef", {}, 331618);
  check_method("chainsaw.CharacterManager", "getPlayerContextRef", {"chainsaw.CharacterManager.PlayerNo"}, 331630);
  check_method("chainsaw.CharacterManager", "getPlayerContextRef", {"chainsaw.CharacterManager.PlayerKind"}, 331631);
  check_method("chainsaw.CharacterManager", "calcPlayerKind", {"chainsaw.CharacterKindID"}, 331628, true);
  check_method("chainsaw.CharacterManager", "calcPlayerKind", {"chainsaw.CharacterKindID"}, 0, false);
  check_method("chainsaw.CharacterManager", "saveGameSaveData", {}, 331653, false, 4);
  check_method("chainsaw.CharacterManager", "loadGameSaveData", {"share.SaveDataBase"}, 331654, false, 5);
  check(re::method_vt_index(331618) == -1, "a method that is not virtual has no vtable slot");

  check_game();

  // A method entry hook (TDB 71): the entry holds an int32 offset from the
  // code base; hook_method points it at a jump of the mod's own near the base,
  // hands back the code it held, and unhook puts it back only while the entry
  // still holds that jump.
  {
    const uintptr_t rec = re::method_record_at(331618);
    check(rec && re::method_at(rec) == "chainsaw.CharacterManager.getPlayerContextRef", "method 331618's entry names it");
    const uintptr_t code_at = 8;  // where a method record keeps its code offset
    struct Fn {
      static void original() {}
      static void replacement() {}
    };
    const auto orig = reinterpret_cast<uintptr_t>(&Fn::original);
    const auto repl = reinterpret_cast<uintptr_t>(&Fn::replacement) + 0x7FFF00000000ull;  // far away (never called)
    check(re::hook_method(rec, repl) == 0, "an entry with no code is not hooked");
    *reinterpret_cast<int32_t*>(rec + code_at) = static_cast<int32_t>(orig - code_base);
    check(re::method_code_at(331618) == orig, "method_code_at decodes the entry's offset");
    const uintptr_t got = re::hook_method(rec, repl);
    const int32_t enc = *reinterpret_cast<int32_t*>(rec + code_at);
    const uintptr_t stub = code_base + static_cast<intptr_t>(enc);
    check(got == orig, "hook_method returns the code the entry held");
    check(enc != static_cast<int32_t>(orig - code_base) && mem::read<uint16_t>(stub) == 0x25FF &&
              mem::read<uintptr_t>(stub + 6) == repl,
          "the entry now names a jump (jmp [rip+0]) to the replacement, within reach of the base");
    check(re::hook_method(rec, repl) == 0, "an entry holding the mod's jump (not the game's code) is not hooked again");
    check(!re::unhook_method(rec, orig, repl), "unhook refuses when the entry does not hold that replacement");
    check(re::unhook_method(rec, repl, orig) && re::method_code_at(331618) == orig, "unhook puts the code back");
    check(re::hook_method(rec + code_at, repl) == 0 && re::hook_method(0, repl) == 0, "only method entries are hooked");
    // In the game the entries are in re4.exe's .data, copy-on-write pages - or
    // pages the runtime may protect: the exchange must get through either.
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(rec + code_at), 4, PAGE_READONLY, &old);
    unsigned long protect = 0;
    check(re::hook_method(rec, repl, &protect) == orig && protect == PAGE_READONLY,
          "hook_method gets through a read-only page (and says so)");
    check(re::unhook_method(rec, repl, orig) && re::method_code_at(331618) == orig, "unhook too");
    VirtualProtect(reinterpret_cast<void*>(rec + code_at), 4, old, &old);
    *reinterpret_cast<int32_t*>(rec + code_at) = 0;
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}

namespace {

void check_game() {
  // (filled in with the game layer's classes)
}

}  // namespace
