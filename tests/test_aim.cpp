// Steady aim's camera (game.cpp: player_camera, steady_camera_controller,
// drop_hand_shake, camera_aiming) over objects shaped like the game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_aim.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_aim.exe
//   wine tests/build/test_aim.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then the player's camera is built the way the VM
// lays out managed objects (as test_stats does): a CharacterContext whose
// CameraController is a MainCameraController, its _CameraControllerArray holding
// a PlayerCameraController, that controller's _MainCameraController (declared on
// CameraControllerRoot), its recoil copy (CameraRecoilParam) and its state
// (PlayerCameraStateParam) - every field placed by name from the real database.
// The hooks on the camera's update and shake rate (events.cpp) rely on these:
// the recoil and the stick lerp zeroed, the hand shake handed to the main camera
// taken back, and the states the camera aims in (the game's checkHandShake, the
// shotgun's pump). And the game's code the hooks on the hold's sway lead (found by
// name, read in the mapped exe): each shake action asks the equipment's optical
// scope check before it sets its layer's blend to the camera's rate.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "../src/game.h"
#include "../src/log.h"
#include "../src/mem.h"
#include "../src/re.h"

namespace {

using namespace re4cc;

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

HMODULE g_game = nullptr;
using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
GetModuleHandleAFn g_real_gmh = nullptr;
HMODULE WINAPI game_module(LPCSTR name) { return name ? g_real_gmh(name) : g_game; }

uintptr_t alloc(size_t n) {
  return reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

// One REObjectInfo per class: the fields start 0x10 into the object.
std::map<uint32_t, uintptr_t> g_infos;
uintptr_t info_of(uint32_t t) {
  auto it = g_infos.find(t);
  if (it != g_infos.end()) return it->second;
  const uintptr_t b = alloc(16);
  *reinterpret_cast<int32_t*>(b) = 0x10;
  *reinterpret_cast<uintptr_t*>(b + 8) = re::type_record(t);
  return g_infos[t] = b + 8;
}

uintptr_t make(const char* type_name) {
  const uint32_t t = re::find_type(type_name);
  if (!t || !re::type_record(t)) {
    std::printf("FAIL  no class %s\n", type_name);
    ++g_fail;
    return 0;
  }
  const uintptr_t o = alloc(0x200);
  *reinterpret_cast<uintptr_t*>(o) = info_of(t);
  return o;
}

uintptr_t at(uintptr_t obj, const char* field) {
  const re::Field f = re::find_field(re::type_of(obj), field);
  const uintptr_t a = re::field_addr(obj, f);
  if (!a) {
    std::printf("FAIL  no field %s on %s\n", field, re::full_name(re::type_of(obj)).c_str());
    ++g_fail;
  }
  return a;
}
template <typename T>
void set(uintptr_t obj, const char* field, T v) {
  if (const uintptr_t a = at(obj, field)) *reinterpret_cast<T*>(a) = v;
}
template <typename T>
T get(uintptr_t obj, const char* field) {
  const uintptr_t a = at(obj, field);
  return a ? *reinterpret_cast<T*>(a) : T{};
}

uintptr_t array_of(const char* elem_type, std::initializer_list<uintptr_t> items) {
  const uintptr_t arr = alloc(0x20 + 8 * items.size());
  *reinterpret_cast<uintptr_t*>(arr + 0x10) = re::type_record(re::find_type(elem_type));
  *reinterpret_cast<int32_t*>(arr + 0x1C) = static_cast<int32_t>(items.size());
  int i = 0;
  for (const uintptr_t it : items) reinterpret_cast<uintptr_t*>(arr + 0x20)[i++] = it;
  return arr;
}

int enum_of(const char* type, const char* literal) { return re::enum_value(re::find_type(type), literal, -1); }

// A method's code in the mapped exe, up to the ret the compiler padded after it.
struct Code {
  uintptr_t at = 0;
  size_t n = 0;
  uint8_t u8(size_t i) const { return mem::read<uint8_t>(at + i); }
  int32_t i32(size_t i) const { return mem::read<int32_t>(at + i); }
  // The first direct call (E8) or jump (E9) to `target` from `from` on: its offset, or -1.
  long branch_to(uintptr_t target, size_t from = 0, uint8_t op = 0xE8) const {
    for (size_t i = from; i + 5 <= n; ++i)
      if (u8(i) == op && at + i + 5 + static_cast<intptr_t>(i32(i + 1)) == target) return static_cast<long>(i);
    return -1;
  }
  // The first virtual call through `slot` of a table from `from` on - the slot read
  // (mov r64,[reg+slot*8]) then a call through a register: its offset, or -1.
  long virtual_call(int slot, size_t from = 0) const {
    for (size_t i = from; i + 7 <= n; ++i) {
      if ((u8(i) & 0xF8) != 0x48 || u8(i + 1) != 0x8B || (u8(i + 2) & 0xC0) != 0x80 || (u8(i + 2) & 7) == 4 ||
          i32(i + 3) != slot * 8)
        continue;
      for (size_t k = i + 7; k + 2 <= n && k < i + 7 + 12; ++k)
        if (u8(k) == 0xFF && (u8(k + 1) & 0xF8) == 0xD0) return static_cast<long>(i);
    }
    return -1;
  }
};
Code code_of(const char* type, const char* method, std::initializer_list<const char*> params) {
  Code c;
  const uint32_t t = re::find_type(type);
  c.at = t ? re::method_code_at(re::find_method(t, method, params)) : 0;
  while (c.at && c.n < 0x1000 && !(c.u8(c.n) == 0xC3 && c.u8(c.n + 1) == 0xCC)) ++c.n;
  if (c.at) ++c.n;
  if (!c.at) {
    std::printf("FAIL  no code for %s.%s\n", type, method);
    ++g_fail;
  }
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_aim.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_aim_" + std::to_string(GetCurrentProcessId()) + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  log_init(dir.c_str());

  // --- the game's image and database, as test_discover sets them up ---
  g_game = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!g_game) {
    std::printf("could not map %s (%lu)\n", argv[1], GetLastError());
    return 1;
  }
  const auto base = reinterpret_cast<uintptr_t>(g_game);
  const mem::Range data = mem::section(g_game, ".data");
  uintptr_t tdb = 0;
  for (uintptr_t a = (data.begin + 7) & ~static_cast<uintptr_t>(7); a + 8 < data.end; a += 8)
    if (!std::memcmp(reinterpret_cast<const void*>(a), "TDB\0", 4) && mem::read<uint32_t>(a + 4) == 71) {
      tdb = a;
      break;
    }
  if (!tdb) {
    std::printf("no TDB v71 in %s\n", argv[1]);
    return 1;
  }
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(data.begin), data.size(), PAGE_READWRITE, &old);
  for (int i = 0; i < 18; ++i) {
    auto* p = reinterpret_cast<uint64_t*>(tdb + 0x58 + 8 * i);
    if (*p) *p += tdb;
  }
  const uintptr_t methods = mem::read<uint64_t>(tdb + 0x70);
  const uint32_t n_methods = mem::read<uint32_t>(tdb + 0x10);
  const auto decoders = mem::find_all(mem::section(g_game, ".text"), "85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 03 C1 C3", 2);
  if (decoders.size() != 1) {
    std::printf("%u method-offset decoders in %s\n", static_cast<unsigned>(decoders.size()), argv[1]);
    return 1;
  }
  const uintptr_t code_base = mem::rip_target(decoders[0] + 10, 3, 7);
  FILE* f = std::fopen(argv[2], "rb");
  uint32_t header[4] = {};
  if (!f || std::fread(header, sizeof(header), 1, f) != 1 || header[0] != 0x4D344552 || header[2] != n_methods) {
    std::printf("%s is not a method map of this exe\n", argv[2]);
    return 1;
  }
  std::vector<uint32_t> rvas(n_methods);
  std::fread(rvas.data(), 4, rvas.size(), f);
  std::fclose(f);
  for (uint32_t i = 0; i < n_methods; ++i)
    if (rvas[i])
      *reinterpret_cast<int32_t*>(methods + static_cast<uintptr_t>(i) * 12 + 8) =
          static_cast<int32_t>(static_cast<intptr_t>(base + rvas[i]) - static_cast<intptr_t>(code_base));
  g_real_gmh = reinterpret_cast<GetModuleHandleAFn>(
      mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&game_module)));
  if (!g_real_gmh) {
    std::printf("could not redirect GetModuleHandleA\n");
    return 1;
  }
  // The stand-in VM: the database pointer and a static table the test fills.
  uintptr_t vm_slot = 0;
  {
    std::map<uintptr_t, int> seen;
    for (const uintptr_t s : mem::find_all(mem::section(g_game, ".text"), "48 8B 0D ?? ?? ?? ?? BA FF FF FF FF E8", 4096))
      if (++seen[mem::rip_target(s, 3, 7)] > 10) {
        vm_slot = mem::rip_target(s, 3, 7);
        break;
      }
  }
  if (!vm_slot) {
    std::printf("no VM global in %s\n", argv[1]);
    return 1;
  }
  const uint32_t n_types = mem::read<uint32_t>(tdb + 0x0C);
  auto* vm = reinterpret_cast<uint8_t*>(alloc(0x10000));
  auto* statics = reinterpret_cast<uintptr_t*>(alloc(static_cast<size_t>(n_types) * 8));
  *reinterpret_cast<uintptr_t*>(vm + 0x3000) = tdb;
  *reinterpret_cast<uintptr_t**>(vm + 0x3000 - 0x30) = statics;
  *reinterpret_cast<uint32_t*>(vm + 0x3000 - 0x28) = n_types;
  *reinterpret_cast<uint8_t**>(vm_slot) = vm;
  bool ready = re::init();
  for (int tries = 0; !ready && tries < 5 && !re::unsupported(); ++tries) ready = re::init();
  if (!ready) {
    std::printf("re::init: %s\n", re::status());
    return 1;
  }
  game::discover();
  check(game::parts().aim && game::parts().aim_camera, "discovery: the guns' aim and the camera's shakes");

  // --- the player's camera ---
  const uintptr_t ctx = make("chainsaw.CharacterContext");
  const uintptr_t mcc = make("chainsaw.MainCameraController");
  const uintptr_t pcc = make("chainsaw.PlayerCameraController");
  const uintptr_t recoil = make("chainsaw.CameraRecoilParam");
  const uintptr_t state = make("chainsaw.PlayerCameraStateParam");
  set<uintptr_t>(ctx, "<CameraController>k__BackingField", mcc);
  set<uintptr_t>(mcc, "_CameraControllerArray", array_of("chainsaw.CameraControllerRoot", {pcc}));
  set<uintptr_t>(pcc, "_MainCameraController", mcc);
  set<uintptr_t>(pcc, "_RecoilParam", recoil);
  set<uintptr_t>(pcc, "_CurrentStateParam", state);

  check(game::player_camera(ctx) == pcc, "the player's camera controller: the context's main camera's first");
  check(game::player_camera(mcc) == 0, "something that is no context: none");

  // The recoil copy and the stick lerp, zeroed.
  set<float>(recoil, "_Yaw", 1.5f);
  set<float>(recoil, "_Pitch", -0.75f);
  set<float>(pcc, "_OldRSOffset", 0.25f);
  set<float>(pcc, "_RSOffsetLerp", 0.5f);
  check(game::steady_camera(ctx), "the camera steadied through the context");
  check(get<float>(recoil, "_Yaw") == 0.0f && get<float>(recoil, "_Pitch") == 0.0f, "the shot's recoil copy held at 0");
  check(get<float>(pcc, "_OldRSOffset") == 0.0f && get<float>(pcc, "_RSOffsetLerp") == 0.0f,
        "the hand shake's stick lerp held at 0");
  check(!game::steady_camera_controller(pcc), "nothing left to write: no write");
  set<float>(pcc, "_RSOffsetLerp", 0.033f);
  check(game::steady_camera_controller(pcc) && get<float>(pcc, "_RSOffsetLerp") == 0.0f,
        "the lerp a frame's reads moved on, held at 0 again (the hooks' own call)");
  check(!game::steady_camera_controller(mcc), "another controller class: left alone");

  // The hand shake the camera's update hands its main camera.
  set<bool>(mcc, "_IsHandShake", true);
  check(game::drop_hand_shake(pcc) && !get<bool>(mcc, "_IsHandShake"), "the hand shake handed over is taken back");
  check(!game::drop_hand_shake(pcc), "none handed over: nothing to take back");
  set<bool>(mcc, "_IsHandShake", true);
  check(!game::drop_hand_shake(mcc) && get<bool>(mcc, "_IsHandShake"), "not a player camera controller: left alone");
  set<bool>(mcc, "_IsHandShake", false);

  // The states the camera aims in.
  const char* states = "chainsaw.CameraDefine.PlayerCameraState";
  auto aiming = [&](const char* literal, const char* gimmick = nullptr) {
    set<int32_t>(state, "<State>k__BackingField", enum_of(states, literal));
    set<int32_t>(state, "<GimmickType>k__BackingField", gimmick ? enum_of("chainsaw.CameraDefine.GimmickType", gimmick) : 0);
    return game::camera_aiming(pcc);
  };
  check(aiming("Hold") && aiming("HoldIronSight") && aiming("HoldOpticalScope") && aiming("HoldSpecialOpticalScope") &&
            aiming("HoldExtraScope") && aiming("HoldVariation"),
        "aiming: the hold states (Hold to HoldExtraScope, HoldVariation)");
  check(aiming("PumpAction"), "aiming: the shotgun's pump");
  check(aiming("Gimmick", "InstalledMachineGun"), "aiming: a mounted machine gun");
  check(!aiming("Gimmick", "FallenShelf"), "not aiming: another gimmick");
  check(!aiming("Normal") && !aiming("Sprint") && !aiming("ViaScope") && !aiming("HoldGrenade") && !aiming("Damage"),
        "not aiming: walking, running, the scope's way in, a grenade, a hit");
  set<uintptr_t>(pcc, "_CurrentStateParam", 0);
  check(!game::camera_aiming(pcc), "no state: not aiming");

  // --- the hold's sway: the game's code the hooks lead (events.cpp kHoldShake, kScopeShake, kOpticScope) ---
  // Each shake action's onUpdate asks the equipment's get_IsEnableOpticalScope
  // through its table, then reads the camera's getHandShakeRate and sets its
  // layer's blend to it - TreeLayer.set_BlendRate's own code (a jump to the
  // setter of the layer's blend), inlined. Its onEnd sets the blend back.
  const int scope_slot =
      re::method_vt_index(re::find_method(re::find_type("chainsaw.PlayerEquipment"), "get_IsEnableOpticalScope", {}));
  const uintptr_t rate_code = re::method_code_at(
      re::find_method(re::find_type("chainsaw.PlayerCameraController"), "getHandShakeRate", {}));
  const Code set_blend = code_of("via.motion.TreeLayer", "set_BlendRate", {"System.Single"});
  uintptr_t blend_setter = 0;
  for (size_t i = 0; i + 5 <= set_blend.n && i < 16 && !blend_setter; ++i)
    if (set_blend.u8(i) == 0xE9) blend_setter = set_blend.at + i + 5 + static_cast<intptr_t>(set_blend.i32(i + 1));
  check(scope_slot > 0 && rate_code && blend_setter,
        "the equipment's scope check is virtual, the camera's rate and the layer's blend setter found");
  for (const char* action :
       {"chainsaw.PlayerBehaviorTreeAction_MFSM_HoldShake", "chainsaw.PlayerBehaviorTreeAction_MFSM_ScopeDangerHoldShake"}) {
    const Code update = code_of(action, "onUpdate", {"via.behaviortree.ActionArg"});
    const long asks = update.virtual_call(scope_slot);
    const long rate = asks < 0 ? -1 : update.branch_to(rate_code, static_cast<size_t>(asks));
    const long blend = rate < 0 ? -1 : update.branch_to(blend_setter, static_cast<size_t>(rate));
    std::string what = std::string(action + 9) + ".onUpdate: asks the scope check, then sets the blend to the rate";
    check(asks >= 0 && rate > asks && blend > rate, what.c_str());
    const Code end = code_of(action, "onEnd", {"via.behaviortree.ActionArg"});
    what = std::string(action + 9) + ".onEnd: sets the blend back";
    check(end.branch_to(blend_setter) >= 0, what.c_str());
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
