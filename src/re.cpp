#include "re.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "mem.h"

namespace re4cc::re {
namespace {

// --- the database's layout (TDB 71) ------------------------------------------------------
// The current Steam build (22377325) has TDB 71 (REFramework's tdb71::TDB,
// RETypeDefVersion71): counts at 0x0C.., 18 array pointers from 0x58, rewritten
// from offsets to addresses by the VM as it starts. Names, counts and flags live
// in separate "impl" records the typedefs, fields and methods point at; 19-bit
// type indices. Unlike TDB 70 (RE2, RE3) a method's entry holds no code pointer
// but an int32 `encoded_offset` from a fixed base in .text, which the VM fills in
// as it links the game's code (0 on disk). Confirmed on re4.exe: every array
// ends where the next begins, every typedef names itself, and fields, methods,
// enum literals and generic names read back as tools/tdb_dump.py reads them.
constexpr uint32_t kMagic = 0x00424454;  // "TDB\0"
constexpr uint32_t kVersion = 71;        // build 22377325

bool supported_version(uint32_t v) { return v == kVersion; }

// RETypeDefVersion71 (0x48 bytes).
struct TypeDef {
  uint64_t a;  // index:19 parent:19 declaring:19 underlying:7
  uint64_t b;  // array:19 element:19 impl:18 system:7
  uint32_t flags, size, fqn, crc;
  uint64_t c;     // default_ctor:22 member_method:22 member_field:19
  uint32_t prop;  // num_member_prop:12 member_prop:19
  uint32_t e;     // ?:26 object_type:3
  int64_t g;      // ?:26 generics:26 interfaces:12
  uintptr_t clr_type;
  uintptr_t managed_vt;  // the REObjectInfo of its instances
};
static_assert(sizeof(TypeDef) == 0x48, "TypeDef");

#pragma pack(push, 4)
struct TypeImpl {
  int32_t name, ns, field_size, static_size;
  uint64_t w;  // ?:33 num_member_fields:24 ?:7
  uint16_t num_methods;
  int16_t num_native_vt, iface;
  uint8_t pad[0x12];
};
struct MethodImpl {
  uint16_t attrs;
  int16_t vt_index;
  uint16_t flags, impl_flags;
  uint32_t name;
};
struct FieldImpl {
  uint16_t attrs, flags;  // flags: ?:1 flags:15
  uint32_t w1;  // offset:26 init_lo:6
  uint32_t w2;  // name:28 init_mid:4
};
struct ParamDef {
  uint16_t attrs, init;
  uint32_t w1;  // name:30 modifier:2
  uint32_t w2;  // type:19 flags:13
};
struct MethodDef {
  uint32_t a;  // declaring:19 params_lo:13
  uint32_t b;  // impl:19 params_hi:13
  int32_t encoded_offset;  // the code, from the decoder's base (0: none, or not linked yet)
};
#pragma pack(pop)
static_assert(sizeof(TypeImpl) == 0x30, "TypeImpl");
static_assert(sizeof(MethodImpl) == 12, "MethodImpl");
static_assert(sizeof(FieldImpl) == 12, "FieldImpl");
static_assert(sizeof(ParamDef) == 12, "ParamDef");
static_assert(sizeof(MethodDef) == 12, "MethodDef");

struct Db {
  uintptr_t base = 0;
  uint32_t version = 0;
  uint32_t n_types = 0, n_methods = 0, n_fields = 0, n_type_impl = 0, n_method_impl = 0, n_field_impl = 0;
  uint32_t n_params = 0, n_init = 0, n_strings = 0, n_bytes = 0;
  uintptr_t types = 0, types_impl = 0, methods = 0, methods_impl = 0, fields = 0, fields_impl = 0;
  uintptr_t params = 0, init_data = 0, strings = 0, bytes = 0;
  uint32_t string_mask = 0;  // a name offset's own bits: the string pool's size rounded up to a power of two, less 1
  // The records' sizes.
  uint32_t type_size = sizeof(TypeDef), method_size = sizeof(MethodDef), field_size = 8;
} g;

// The base every method's encoded_offset counts from: the RIP-relative operand
// of the engine's decoder, `test ecx,ecx; jne +3; xor eax,eax; ret; movsxd rax,ecx;
// lea rcx,[base]; add rax,rcx; ret` (REFramework's get_encoded_pointer), found
// in .text by init(). 0 until then.
uintptr_t g_code_base = 0;

uintptr_t g_vm_slot = 0;
uintptr_t g_static_elems = 0;  // uint8_t** - one static block per type index
uint32_t g_static_size = 0;
std::atomic<bool> g_ready{false};  // set last, on the mod thread; the tick reads it
std::atomic<bool> g_unsupported{false};  // re4.exe has no TDB the mod reads: init() never will succeed
char g_status[320] = "not started";

std::vector<std::string> g_names;      // full names, built once
std::unordered_map<std::string, uint32_t> g_by_name;
// A List<T> class's two fields, found by name the first time a list of the
// class is read (list_fields): one word per slot - the class, and the offsets of
// _items and _size (or none) - read and written whole, so the tick and the hooks
// on the game's other threads share it without a lock. A class whose slot
// another took is looked up again.
constexpr uint32_t kListSlots = 256, kNoOffset = 0x3FFFFF;
std::atomic<uint64_t> g_list_fields[kListSlots];

const char* str(uint32_t off) {
  off &= g.string_mask;
  return off < g.n_strings ? reinterpret_cast<const char*>(g.strings + off) : "";
}

// --- a typedef, a field and a method, read through their impl records ----------------------
uintptr_t td_at(uint32_t i) { return g.types + static_cast<uintptr_t>(i) * g.type_size; }
const TypeDef* td(uint32_t i) { return reinterpret_cast<const TypeDef*>(td_at(i)); }
const TypeImpl* timpl(uint32_t i) {
  return i < g.n_type_impl ? reinterpret_cast<const TypeImpl*>(g.types_impl + static_cast<uintptr_t>(i) * sizeof(TypeImpl)) : nullptr;
}
uint32_t td_impl_index(uint32_t i) { return static_cast<uint32_t>((td(i)->b >> 38) & 0x3FFFF); }
const TypeImpl* td_impl(uint32_t i) { return timpl(td_impl_index(i)); }

uint32_t td_index(uint32_t i) { return static_cast<uint32_t>(td(i)->a & 0x7FFFF); }
uint32_t td_parent(uint32_t i) { return static_cast<uint32_t>((td(i)->a >> 19) & 0x7FFFF); }
uint32_t td_declaring(uint32_t i) { return static_cast<uint32_t>((td(i)->a >> 38) & 0x7FFFF); }
uint32_t td_element(uint32_t i) { return static_cast<uint32_t>((td(i)->b >> 19) & 0x7FFFF); }
uint32_t td_objtype(uint32_t i) {  // via.clr's object type: 1 class, 3 string, 5 value type
  return (td(i)->e >> 26) & 7;
}
int32_t td_generics(uint32_t i) {  // a 26-bit signed byte pool offset
  const uint32_t v = static_cast<uint32_t>((static_cast<uint64_t>(td(i)->g) >> 26) & 0x3FFFFFF);
  return static_cast<int32_t>(v << 6) >> 6;
}
uintptr_t td_managed_vt(uint32_t i) { return td(i)->managed_vt; }
const char* td_name(uint32_t i) {
  const TypeImpl* ti = td_impl(i);
  return ti ? str(static_cast<uint32_t>(ti->name)) : "";
}
const char* td_namespace(uint32_t i) {
  const TypeImpl* ti = td_impl(i);
  return ti ? str(static_cast<uint32_t>(ti->ns)) : "";
}
uint32_t td_field_size(uint32_t i) {  // the fields' size: a value type's in an array
  const TypeImpl* ti = td_impl(i);
  return ti ? static_cast<uint32_t>(ti->field_size) : 0;
}
uint32_t first_field_of(uint32_t t) { return static_cast<uint32_t>((td(t)->c >> 44) & 0x7FFFF); }
uint32_t num_fields_of(uint32_t t) {
  const TypeImpl* ti = td_impl(t);
  return ti && first_field_of(t) ? static_cast<uint32_t>((ti->w >> 33) & 0xFFFFFF) : 0;
}
uint32_t first_method_of(uint32_t t) { return static_cast<uint32_t>((td(t)->c >> 22) & 0x3FFFFF); }
uint32_t num_methods_of(uint32_t t) {
  const TypeImpl* ti = td_impl(t);
  return ti && first_method_of(t) ? ti->num_methods : 0;
}

struct FieldView {
  bool ok = false;
  uint32_t declaring = 0, type = 0, offset = 0, init = 0;
  uint16_t flags = 0;
  const char* name = "";
};
FieldView field_view(uint32_t i) {
  FieldView v;
  if (i >= g.n_fields) return v;
  const uint64_t raw = *reinterpret_cast<const uint64_t*>(g.fields + static_cast<uintptr_t>(i) * 8);
  const uint32_t impl = static_cast<uint32_t>((raw >> 19) & 0x7FFFF);
  if (impl >= g.n_field_impl) return v;
  const auto* fi = reinterpret_cast<const FieldImpl*>(g.fields_impl + static_cast<uintptr_t>(impl) * sizeof(FieldImpl));
  v.ok = true;
  v.declaring = static_cast<uint32_t>(raw & 0x7FFFF);
  v.type = static_cast<uint32_t>((raw >> 38) & 0x7FFFF);
  v.offset = fi->w1 & 0x3FFFFFF;
  // The init data index, split three ways: 6 bits in the impl's offset word, 4 in
  // its name word, 6 in the field's own record.
  v.init = (fi->w1 >> 26) | ((fi->w2 >> 28) << 6) | (static_cast<uint32_t>((raw >> 57) & 0x3F) << 10);
  v.flags = static_cast<uint16_t>(fi->flags >> 1);
  v.name = str(fi->w2 & 0xFFFFFFF);
  return v;
}

struct MethodView {
  bool ok = false;
  uint32_t declaring = 0, nparams = 0, params = 0;  // params: the byte pool offset of the parameter list
  uint16_t flags = 0;
  int16_t vt_index = -1;
  const char* name = "";
};
uintptr_t method_at_index(uint32_t i) { return g.methods + static_cast<uintptr_t>(i) * g.method_size; }
MethodView method_view(uint32_t i) {
  MethodView v;
  if (i >= g.n_methods) return v;
  const auto* md = reinterpret_cast<const MethodDef*>(method_at_index(i));
  const uint32_t impl = md->b & 0x7FFFF;
  if (impl >= g.n_method_impl) return v;
  const auto* mi = reinterpret_cast<const MethodImpl*>(g.methods_impl + static_cast<uintptr_t>(impl) * sizeof(MethodImpl));
  v.ok = true;
  v.declaring = md->a & 0x7FFFF;
  v.params = ((md->b >> 19) << 13) | (md->a >> 19);
  // The byte pool's ParamList: {u16 count; u16 invoke id; u32 return; u32 params[count]}.
  v.nparams = v.params + 2 <= g.n_bytes ? mem::read<uint16_t>(g.bytes + v.params) : 0xFFFF;
  v.flags = mi->flags;
  v.vt_index = mi->vt_index;
  v.name = str(mi->name);
  return v;
}
// The class of parameter k of a method (0 when it cannot be read).
uint32_t param_type(const MethodView& m, uint32_t k) {
  if (k >= m.nparams) return 0;
  if (m.params + 8 + (k + 1) * 4ull > g.n_bytes) return 0;
  const uint32_t p = mem::read<uint32_t>(g.bytes + m.params + 8 + k * 4ull);
  if (p >= g.n_params) return 0;
  return reinterpret_cast<const ParamDef*>(g.params + static_cast<uintptr_t>(p) * sizeof(ParamDef))->w2 & 0x7FFFF;
}
// A method's code: its encoded_offset from the decoder's base (0: no code, or
// not linked yet).
int32_t encoded_of(uint32_t i) { return i < g.n_methods ? reinterpret_cast<const MethodDef*>(method_at_index(i))->encoded_offset : 0; }
uintptr_t code_of(uint32_t i) {
  const int32_t enc = encoded_of(i);
  return enc && g_code_base ? g_code_base + static_cast<intptr_t>(enc) : 0;
}

// The header, read from a candidate address. The pointers are addresses once the
// VM has initialised it, offsets from the header in the exe's own copy.
bool read_header(uintptr_t base, Db* out) {
  if (!mem::readable(base, 0xE8)) return false;
  const auto u32 = [&](int o) { return mem::read<uint32_t>(base + o); };
  const auto u64 = [&](int o) { return mem::read<uint64_t>(base + o); };
  if (u32(0) != kMagic || !supported_version(u32(4))) return false;
  Db d;
  d.base = base;
  d.version = u32(4);
  d.n_types = u32(0x0C);
  d.n_methods = u32(0x10);
  d.n_fields = u32(0x14);
  d.n_type_impl = u32(0x18);
  d.n_field_impl = u32(0x1C);
  d.n_method_impl = u32(0x20);
  d.n_params = u32(0x30);
  d.n_init = u32(0x38);
  d.n_strings = u32(0x50);
  d.n_bytes = u32(0x54);
  d.types = u64(0x60);
  d.types_impl = u64(0x68);
  d.methods = u64(0x70);
  d.methods_impl = u64(0x78);
  d.fields = u64(0x80);
  d.fields_impl = u64(0x88);
  d.params = u64(0xA8);
  d.init_data = u64(0xB8);
  d.strings = u64(0xD0);
  d.bytes = u64(0xD8);
  if (d.n_types > 0x7FFFF) return false;
  if (!d.n_types || !d.n_methods || !d.n_fields || !d.n_strings || !d.n_bytes) return false;
  // Names carry other data above their own bits (REFramework's string pool bitmask).
  uint32_t mask = 1;
  while (mask < d.n_strings && mask < 0x80000000u) mask <<= 1;
  d.string_mask = mask - 1;
  *out = d;
  return true;
}

// The header's array pointers: none may still be an offset once the VM has
// loaded the database.
constexpr int kFirstArray = 0x58, kArrays = 18;

// Every array the mod reads must be where the header says, whole.
bool arrays_readable(const Db& d) {
  const bool common = mem::readable(d.types, static_cast<size_t>(d.n_types) * d.type_size) &&
                      mem::readable(d.methods, static_cast<size_t>(d.n_methods) * d.method_size) &&
                      mem::readable(d.fields, static_cast<size_t>(d.n_fields) * d.field_size) &&
                      mem::readable(d.init_data, static_cast<size_t>(d.n_init) * 4) &&
                      mem::readable(d.strings, d.n_strings) && mem::readable(d.bytes, d.n_bytes);
  return common && mem::readable(d.types_impl, static_cast<size_t>(d.n_type_impl) * sizeof(TypeImpl)) &&
         mem::readable(d.methods_impl, static_cast<size_t>(d.n_method_impl) * sizeof(MethodImpl)) &&
         mem::readable(d.fields_impl, static_cast<size_t>(d.n_field_impl) * sizeof(FieldImpl)) &&
         mem::readable(d.params, static_cast<size_t>(d.n_params) * sizeof(ParamDef));
}

// Every typedef names itself: its index field is its own position. A database
// read with the wrong layout fails this at once.
bool self_check(char* why, size_t n) {
  const uint32_t probes[] = {1, 2, 3, 100, 1000, g.n_types / 2, g.n_types - 1};
  for (uint32_t i : probes) {
    if (i == 0 || i >= g.n_types) continue;
    const uint32_t idx = td_index(i);
    if (idx != i) {
      std::snprintf(why, n, "typedef %u says it is %u", i, idx);
      return false;
    }
    if (!td_impl(i)) {
      std::snprintf(why, n, "typedef %u has impl %u of %u", i, td_impl_index(i), g.n_type_impl);
      return false;
    }
    if (!*td_name(i)) {
      std::snprintf(why, n, "typedef %u has no name", i);
      return false;
    }
  }
  return true;
}

const std::string& build_name(uint32_t i, int depth) {
  static const std::string kEmpty;
  if (i == 0 || i >= g.n_types) return kEmpty;
  std::string& slot = g_names[i];
  if (!slot.empty() || depth > 16) return slot;
  const char* ns = td_namespace(i);
  const char* nm = td_name(i);
  std::string full;
  const uint32_t decl = td_declaring(i);
  const uint32_t elem = td_element(i);
  if (!*nm && elem && elem != i && elem < g.n_types) {
    // An array: the engine names it by reflection, its typedef carries none.
    full = build_name(elem, depth + 1);
    full += "[]";
    slot = full;
    return slot;
  }
  if (decl && decl != i && decl < g.n_types) {
    full = build_name(decl, depth + 1);
    full += '.';
    full += nm;
  } else {
    if (*ns) {
      full = ns;
      full += '.';
    }
    full += nm;
  }
  // A generic instance: "List`1<chainsaw.Item>". Its argument list in the byte
  // pool: {definition:19 num:13; u32 types[num]}.
  const int32_t generics = td_generics(i);
  if (generics > 0 && static_cast<uint32_t>(generics) + 4 <= g.n_bytes) {
    const uintptr_t gl = g.bytes + static_cast<uint32_t>(generics);
    const uint32_t w = mem::read<uint32_t>(gl);
    const uint32_t def = w & 0x7FFFF, num = w >> 19;
    if (def != i && num > 0 && num <= 16 && static_cast<uint32_t>(generics) + 4 + num * 4 <= g.n_bytes) {
      full += '<';
      for (uint32_t k = 0; k < num; ++k) {
        if (k) full += ',';
        const uint32_t a = mem::read<uint32_t>(gl + 4 + 4 * k);
        full += build_name(a, depth + 1);
      }
      full += '>';
    }
  }
  slot = full;
  return slot;
}

bool attach(const Db& d) {
  g = d;
  char why[160] = "";
  if (!arrays_readable(g)) {
    std::snprintf(g_status, sizeof(g_status), "type database arrays not readable");
    return false;
  }
  if (!self_check(why, sizeof(why))) {
    std::snprintf(g_status, sizeof(g_status), "type database failed its self-check: %s", why);
    return false;
  }
  g_names.assign(g.n_types, std::string());
  g_by_name.clear();
  g_by_name.reserve(g.n_types * 2);
  for (uint32_t i = 1; i < g.n_types; ++i) {
    const std::string& n = build_name(i, 0);
    if (!n.empty()) g_by_name.emplace(n, i);
  }
  for (auto& slot : g_list_fields) slot.store(0, std::memory_order_relaxed);
  return true;
}

// The index of the typedef at `cls` (an REObjectInfo's class), 0 when it is none.
uint32_t class_index(uintptr_t cls) {
  if (cls < g.types || cls >= g.types + static_cast<uintptr_t>(g.n_types) * g.type_size) return 0;
  if ((cls - g.types) % g.type_size) return 0;
  return static_cast<uint32_t>((cls - g.types) / g.type_size);
}

// --- finding the VM ---------------------------------------------------------------------
// `mov rcx,[rip+X]; mov edx,-1; call get_thread_context` - the VM global X is
// loaded this way all over the code; REFramework takes the first global seen
// more than ten times.
uintptr_t find_vm_slot(const mem::Range& text, const mem::Range& image) {
  std::unordered_map<uintptr_t, int> seen;
  const auto* p = reinterpret_cast<const uint8_t*>(text.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(text.end) - 12;
  for (const uint8_t* c = p + 7; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 0xBA, end - c));
    if (!c) break;
    if (c[1] == 0xFF && c[2] == 0xFF && c[3] == 0xFF && c[4] == 0xFF && c[5] == 0xE8 && c[-7] == 0x48 &&
        c[-6] == 0x8B && c[-5] == 0x0D) {
      const uintptr_t insn = reinterpret_cast<uintptr_t>(c - 7);
      const uintptr_t slot = mem::rip_target(insn, 3, 7);
      if (image.contains(slot) && ++seen[slot] > 10) return slot;
    }
    ++c;
  }
  return 0;
}

// The base of the methods' encoded_offset: the one decoder in .text,
// `test ecx,ecx; jne +3; xor eax,eax; ret; movsxd rax,ecx; lea rcx,[base];
// add rax,rcx; ret` - `lea`'s target is the base. 0 when it is not there once.
uintptr_t find_code_base(const mem::Range& text) {
  const auto hits = mem::find_all(text, "85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 03 C1 C3", 2);
  if (hits.size() != 1) return 0;
  const uintptr_t base = mem::rip_target(hits[0] + 10, 3, 7);
  return text.contains(base) ? base : 0;
}

// The database in the image, found by its header: magic, version 71.
uintptr_t find_tdb_in_image(const mem::Range& data) {
  const auto* p = reinterpret_cast<const uint8_t*>(data.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(data.end) - 0x100;
  for (const uint8_t* c = p; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 'T', end - c));
    if (!c) break;
    if ((reinterpret_cast<uintptr_t>(c) & 7) == 0 && std::memcmp(c, "TDB\0", 4) == 0 &&
        supported_version(mem::read<uint32_t>(reinterpret_cast<uintptr_t>(c) + 4)))
      return reinterpret_cast<uintptr_t>(c);
    ++c;
  }
  return 0;
}

// The version of a database header of any other version in the image (0: none)
// - the message of a game the mod was not made for. RE Engine has used 49 to the
// 80s.
uint32_t other_tdb_version(const mem::Range& data) {
  const auto* p = reinterpret_cast<const uint8_t*>(data.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(data.end) - 0x100;
  for (const uint8_t* c = p; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 'T', end - c));
    if (!c) break;
    if ((reinterpret_cast<uintptr_t>(c) & 7) == 0 && std::memcmp(c, "TDB\0", 4) == 0) {
      const uint32_t v = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(c) + 4);
      if (v >= 40 && v < 128) return v;
    }
    ++c;
  }
  return 0;
}

}  // namespace

// --- set-up -------------------------------------------------------------------------------
bool attach_tdb(uintptr_t tdb, uintptr_t code_base) {
  Db d;
  if (!read_header(tdb, &d)) return false;
  g_code_base = code_base;
  g_ready = attach(d);
  if (g_ready) std::snprintf(g_status, sizeof(g_status), "ready (%u types)", g.n_types);
  return g_ready;
}

bool init() {
  if (g_ready) return true;
  if (g_unsupported) return false;  // what re4.exe carries does not change
  HMODULE exe = GetModuleHandleA(nullptr);
  const mem::Range image = mem::module_range(exe);
  const mem::Range text = mem::section(exe, ".text");
  const mem::Range data = mem::section(exe, ".data");
  if (text.empty() || data.empty()) {
    std::snprintf(g_status, sizeof(g_status),
                  "re4.exe has no .text/.data section: not a version of the game the mod can read (it works with the "
                  "current Steam version, build 22377325)");
    g_unsupported = true;
    return false;
  }
  static uintptr_t s_image_tdb = 0;
  if (!s_image_tdb) {
    s_image_tdb = find_tdb_in_image(data);
    if (!s_image_tdb) {
      const uint32_t other = other_tdb_version(data);
      if (other)
        std::snprintf(g_status, sizeof(g_status),
                      "re4.exe's type database is v%u, not v71: a version of the game the mod was not made for (it "
                      "works with the current Steam version, build 22377325)",
                      other);
      else
        std::snprintf(g_status, sizeof(g_status),
                      "no type database in re4.exe's .data: not a version of the game the mod can read (it works with "
                      "the current Steam version, build 22377325)");
      g_unsupported = true;
      return false;
    }
    logf("re: TDB v%u header in .data at exe+0x%llX", mem::read<uint32_t>(s_image_tdb + 4),
         static_cast<unsigned long long>(s_image_tdb - image.begin));
  }
  if (!g_code_base) {
    g_code_base = find_code_base(text);
    if (!g_code_base) {
      std::snprintf(g_status, sizeof(g_status),
                    "the decoder of the methods' code offsets was not found in re4.exe: not a version of the game the "
                    "mod can read (it works with the current Steam version, build 22377325)");
      g_unsupported = true;
      return false;
    }
    logf("re: the methods' code offsets count from exe+0x%llX", static_cast<unsigned long long>(g_code_base - image.begin));
  }
  if (!g_vm_slot) {
    g_vm_slot = find_vm_slot(text, image);
    if (!g_vm_slot) {
      std::snprintf(g_status, sizeof(g_status), "the VM global was not found in the code");
      return false;
    }
    logf("re: the VM global is at exe+0x%llX", static_cast<unsigned long long>(g_vm_slot - image.begin));
  }
  const uintptr_t vm = mem::read_ptr(g_vm_slot);
  if (!vm) {
    std::snprintf(g_status, sizeof(g_status), "waiting for the VM to start");
    return false;
  }
  // The VM's pointer to its database - the image's own copy, preferred, or any
  // other header of the same version it holds.
  const uint32_t version = mem::read<uint32_t>(s_image_tdb + 4);
  int tdb_off = -1;
  uintptr_t tdb = 0;
  for (int off = 0x30; off < 0x20000; off += 8) {
    const uintptr_t p = mem::read_ptr(vm + off);
    if (!p || !mem::readable(p, 8) || mem::read<uint32_t>(p) != kMagic || mem::read<uint32_t>(p + 4) != version) continue;
    if (tdb_off < 0 || p == s_image_tdb) {
      tdb_off = off;
      tdb = p;
    }
    if (p == s_image_tdb) break;
  }
  if (tdb_off < 0) {
    std::snprintf(g_status, sizeof(g_status), "waiting for the VM's type database");
    return false;
  }
  // The VM's loader turns every array offset in the header into an address, in
  // place, and sets no flag while it does - the header's `initialized` stays 0. So
  // "loaded" is read off the arrays themselves: none of them may still be an offset.
  for (int i = 0; i < kArrays; ++i) {
    const uint64_t p = mem::read<uint64_t>(tdb + kFirstArray + 8 * static_cast<uintptr_t>(i));
    if (p && p < tdb) {
      std::snprintf(g_status, sizeof(g_status), "waiting for the VM to load the type database (array %d is still an offset)", i);
      return false;
    }
  }
  if (tdb != s_image_tdb)
    logf("re: the VM's type database is at %p, not the image's copy at exe+0x%llX", reinterpret_cast<void*>(tdb),
         static_cast<unsigned long long>(s_image_tdb - image.begin));
  if (g.base != tdb) {
    Db d;
    if (!read_header(tdb, &d)) {
      std::snprintf(g_status, sizeof(g_status), "the type database header does not read as v%u", version);
      return false;
    }
    if (!attach(d)) return false;  // status set
    logf("re: type database v%u at VM+0x%X loaded: %u types, %u methods, %u fields (header flag %u)", g.version, tdb_off,
         g.n_types, g.n_methods, g.n_fields, mem::read<uint32_t>(tdb + 8));
  }
  // The static table: {u8** elements; u32 size}, 0x30 bytes before the database
  // pointer in TDB 70 (REFramework) - and, REFramework expects, in 71. Failing that, REFramework's fix-up for later
  // versions: the first of two {pointer, count} pairs of the same count below it
  // (the static blocks, and the table of which of them are initialised).
  auto table_at = [&](int off, uintptr_t* elems, uint32_t* size) {
    if (off < 0) return false;
    const uintptr_t e = mem::read_ptr(vm + static_cast<uintptr_t>(off));
    uint32_t n = 0;
    if (!e || !mem::read_safe(vm + static_cast<uintptr_t>(off) + 8, &n) || n < 2000 || n > 10000000 ||
        !mem::readable(e, static_cast<size_t>(n) * 8))
      return false;
    *elems = e;
    *size = n;
    return true;
  };
  int static_off = tdb_off - 0x30;
  uintptr_t elems = 0, e2 = 0;
  uint32_t size = 0, n2 = 0;
  if (!table_at(static_off, &elems, &size)) {
    static_off = -1;
    for (int i = 8; i < 0x100 && static_off < 0; i += 8)
      if (table_at(tdb_off - i, &e2, &n2) && table_at(tdb_off - i - 0x10, &elems, &size) && size == n2)
        static_off = tdb_off - i - 0x10;
    if (static_off < 0) {
      std::snprintf(g_status, sizeof(g_status), "the VM's static table was not found near its type database");
      return false;
    }
    logf("re: the static table is not at VM+0x%X - found it at VM+0x%X", tdb_off - 0x30, static_off);
  }
  g_static_elems = elems;
  g_static_size = size;
  // The loader fills the methods' code offsets in after the arrays are rebased,
  // and discovery reads that code: RE2CabbyCodes found on Windows that the mod
  // can get here first and read some of it still 0 (2026-09-25). So wait until
  // the count of methods with code is above 0 and has not moved since the last
  // look (~300 ms).
  static uint32_t s_linked = 0;
  uint32_t linked = 0;
  for (uint32_t i = 0; i < g.n_methods; ++i) linked += encoded_of(i) != 0;
  if (!linked || linked != s_linked) {
    s_linked = linked;
    std::snprintf(g_status, sizeof(g_status), "waiting for the game's code to be linked (%u of %u methods so far)", linked,
                  g.n_methods);
    return false;
  }
  logf("re: VM %p, static table at VM+0x%X (%u entries); %u of %u methods linked", reinterpret_cast<void*>(vm), static_off,
       g_static_size, linked, g.n_methods);
  g_ready = true;
  std::snprintf(g_status, sizeof(g_status), "ready (%u types)", g.n_types);
  return true;
}

bool ready() { return g_ready; }
bool unsupported() { return g_unsupported; }
const char* status() { return g_status; }
uint32_t num_types() { return g.n_types; }
uint32_t num_methods() { return g.n_methods; }
uintptr_t vm() { return g_vm_slot ? mem::read_ptr(g_vm_slot) : 0; }

// --- types ----------------------------------------------------------------------------------
uint32_t find_type(const char* full) {
  if (!g_ready || !full) return 0;
  auto it = g_by_name.find(full);
  return it == g_by_name.end() ? 0 : it->second;
}

const std::string& full_name(uint32_t t) {
  static const std::string kNone = "?";
  return t && t < g.n_types && !g_names.empty() ? g_names[t] : kNone;
}

uint32_t parent(uint32_t t) { return t && t < g.n_types ? td_parent(t) : 0; }

bool is_value_type(uint32_t t) { return t && t < g.n_types && td_objtype(t) == 5; }

bool derives(uint32_t t, uint32_t base) {
  for (int depth = 0; t && t < g.n_types && depth < 64; ++depth, t = td_parent(t))
    if (t == base) return true;
  return false;
}

uint32_t value_size(uint32_t t) { return t && t < g.n_types ? td_field_size(t) : 0; }

uintptr_t managed_vt(uint32_t t) { return t && t < g.n_types ? td_managed_vt(t) : 0; }

uint32_t tdb_version() { return g.version; }

// --- fields ----------------------------------------------------------------------------------
Field find_field(uint32_t t, const char* name) {
  for (int depth = 0; g_ready && t && t < g.n_types && depth < 64; ++depth, t = td_parent(t)) {
    const uint32_t first = first_field_of(t), n = num_fields_of(t);
    for (uint32_t k = 0; k < n; ++k) {
      if (first + k >= g.n_fields) break;
      const FieldView fv = field_view(first + k);
      if (!fv.ok || std::strcmp(fv.name, name) != 0) continue;
      return Field{fv.declaring ? fv.declaring : t, fv.type, fv.offset, fv.flags};
    }
  }
  return Field{};
}

namespace {
// The literal's value: its init data index (split across the field impl's two
// words), an int32 offset into the byte pool, where the value is as wide as the
// enum (`wide`: 8 bytes).
bool literal_value(uint32_t enum_type, const char* name, bool wide, int64_t* out) {
  const Field f = find_field(enum_type, name);
  if (!f.valid() || !(f.flags & 0x40)) return false;  // Literal
  const uint32_t first = first_field_of(f.declaring), n = num_fields_of(f.declaring);
  const uint32_t size = wide ? 8 : 4;
  for (uint32_t k = 0; k < n; ++k) {
    const FieldView fv = field_view(first + k);
    if (!fv.ok || std::strcmp(fv.name, name) != 0) continue;
    const uint32_t index = fv.init;
    if (index >= g.n_init) return false;
    const int32_t off = mem::read<int32_t>(g.init_data + static_cast<uintptr_t>(index) * 4);
    if (off < 0 || static_cast<uint32_t>(off) + size > g.n_bytes) return false;
    *out = wide ? mem::read<int64_t>(g.bytes + static_cast<uint32_t>(off))
                : mem::read<int32_t>(g.bytes + static_cast<uint32_t>(off));
    return true;
  }
  return false;
}
}  // namespace

int enum_value(uint32_t enum_type, const char* name, int fallback) {
  int64_t v = 0;
  return literal_value(enum_type, name, false, &v) ? static_cast<int>(v) : fallback;
}

int64_t enum_value64(uint32_t enum_type, const char* name, int64_t fallback) {
  int64_t v = 0;
  const bool wide = value_size(enum_type) == 8;
  return literal_value(enum_type, name, wide, &v) ? v : fallback;
}

std::string enum_name(uint32_t enum_type, int64_t value) {
  if (!g_ready || !enum_type || enum_type >= g.n_types) return {};
  const bool wide = value_size(enum_type) == 8;
  const uint32_t first = first_field_of(enum_type), n = num_fields_of(enum_type);
  for (uint32_t k = 0; k < n && first + k < g.n_fields; ++k) {
    const FieldView fv = field_view(first + k);
    int64_t v = 0;
    if (fv.ok && (fv.flags & 0x40) && literal_value(enum_type, fv.name, wide, &v) && v == value) return fv.name;
  }
  return {};
}

uintptr_t type_record(uint32_t t) { return g_ready && t && t < g.n_types && td_index(t) == t ? td_at(t) : 0; }

// --- statics -----------------------------------------------------------------------------------
uintptr_t static_block(uint32_t t) {
  if (!g_static_elems || t >= g_static_size) return 0;
  return mem::read_ptr(g_static_elems + static_cast<uintptr_t>(t) * 8);
}

uintptr_t static_addr(const Field& f) {
  if (!f.valid() || !f.is_static()) return 0;
  const uintptr_t block = static_block(f.declaring);
  return block ? block + f.offset : 0;
}

uintptr_t singleton(uint32_t t) {
  const Field f = find_field(t, "_Instance");
  const uintptr_t at = static_addr(f);
  const uintptr_t obj = at ? mem::read_ptr(at) : 0;
  return obj && is_a(obj, t) ? obj : 0;
}

// --- objects ---------------------------------------------------------------------------------------
uint32_t type_of(uintptr_t obj) {
  if (!g_ready) return 0;
  const uintptr_t info = mem::read_ptr(obj);
  return info ? class_index(mem::read_ptr(info)) : 0;
}

bool is_a(uintptr_t obj, uint32_t t) { return t && derives(type_of(obj), t); }

namespace {
// An object's class and where its managed fields begin, from one read of its
// REObjectInfo: the class at +0, and in the int32 just before it REFramework's
// fieldptr offset - the same for every instance of the class. The class is 0
// (the offset -1) unless obj is a managed object whose class the database knows.
uint32_t class_and_fields(uintptr_t obj, int32_t* fieldptr) {
  *fieldptr = -1;
  const uintptr_t info = g_ready ? mem::read_ptr(obj) : 0;
  struct {
    int32_t fieldptr, unused;
    uintptr_t cls;
  } head{};
  if (!info || !mem::copy_from(&head, info - 8, sizeof(head))) return 0;
  const uint32_t t = class_index(head.cls);
  if (t && head.fieldptr >= 0 && head.fieldptr <= 0x1000 && !(head.fieldptr & 7)) *fieldptr = head.fieldptr;
  return t;
}
}  // namespace

int32_t fieldptr_offset(uintptr_t obj) {
  int32_t fp = -1;
  return class_and_fields(obj, &fp) ? fp : -1;
}

uintptr_t field_addr(uintptr_t obj, const Field& f) {
  if (!obj || !f.valid() || f.is_static()) return 0;
  int32_t fp = -1;
  const uint32_t t = class_and_fields(obj, &fp);
  if (!t || fp < 0 || !derives(t, f.declaring)) return 0;
  return obj + static_cast<uint32_t>(fp) + f.offset;
}

// --- arrays and lists ------------------------------------------------------------------------------
// System.Array: the managed header (0x10), the element class at
// +0x10, the count at +0x1C, the elements from +0x20.
bool read_array(uintptr_t arr, Array* out) {
  if (!arr || !mem::readable(arr, 0x20)) return false;
  const uint32_t et = class_index(mem::read_ptr(arr + 0x10));
  const int32_t count = mem::read<int32_t>(arr + 0x1C);
  if (count < 0 || count > 0x100000) return false;
  const uint32_t elem = et && is_value_type(et) ? value_size(et) : 8;
  if (!elem || elem > 0x1000 || !mem::readable(arr + 0x20, static_cast<size_t>(count) * elem)) return false;
  *out = Array{arr, count, arr + 0x20, elem};
  return true;
}

namespace {
// A List<T> class's _items and _size, by their offsets (g_list_fields). One
// List`1 layout for every instantiation, but the offsets are read per class.
// RE4's runtime names them as .NET Core does; RE2's and RE3's had mItems, mSize.
bool list_fields(uint32_t t, uint32_t* items, uint32_t* size) {
  std::atomic<uint64_t>& slot = g_list_fields[t % kListSlots];
  uint64_t v = slot.load(std::memory_order_relaxed);
  if ((v & 0xFFFFF) != t) {
    Field fi = find_field(t, "_items"), fs = find_field(t, "_size");
    if (!fi.valid() || !fs.valid()) {
      fi = find_field(t, "mItems");
      fs = find_field(t, "mSize");
    }
    const bool ok = fi.valid() && fs.valid() && !fi.is_static() && !fs.is_static() && fi.offset < kNoOffset &&
                    fs.offset < kNoOffset;
    v = t | (static_cast<uint64_t>(ok ? fi.offset : kNoOffset) << 20) |
        (static_cast<uint64_t>(ok ? fs.offset : kNoOffset) << 42);
    slot.store(v, std::memory_order_relaxed);
  }
  *items = static_cast<uint32_t>(v >> 20) & kNoOffset;
  *size = static_cast<uint32_t>(v >> 42) & kNoOffset;
  return *items != kNoOffset;
}
}  // namespace

bool read_list(uintptr_t list, List* out) {
  int32_t fp = -1;
  uint32_t items_off = 0, size_off = 0;
  const uint32_t t = class_and_fields(list, &fp);
  if (!t || fp < 0 || !list_fields(t, &items_off, &size_off)) return false;
  const uintptr_t fields = list + static_cast<uint32_t>(fp);
  int32_t size = 0;
  if (!mem::read_safe(fields + size_off, &size) || size < 0) return false;
  List l;
  l.obj = list;
  l.count = size;
  const uintptr_t arr = mem::read_ptr(fields + items_off);
  if (size > 0 && (!read_array(arr, &l.items) || l.items.count < size)) return false;
  *out = l;
  return true;
}

uintptr_t list_ref(const List& l, int i) {
  if (i < 0 || i >= l.count || !l.items.data || l.items.elem_size != 8) return 0;
  return mem::read_ptr(l.items.data + static_cast<uintptr_t>(i) * 8);
}

// --- methods --------------------------------------------------------------------------------------
uintptr_t method_code(uint32_t t, const char* name, int nparams) {
  if (!g_ready || !t || t >= g.n_types) return 0;
  const uint32_t first = first_method_of(t), n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const MethodView m = method_view(first + k);
    if (!m.ok || std::strcmp(m.name, name) != 0) continue;
    if (nparams >= 0 && m.nparams != static_cast<uint32_t>(nparams)) continue;
    return code_of(first + k);
  }
  return 0;
}

uint32_t find_method(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static) {
  if (!g.base || !t || t >= g.n_types) return 0;
  const uint32_t first = first_method_of(t), n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const MethodView m = method_view(first + k);
    if (!m.ok || std::strcmp(m.name, name) != 0) continue;
    if (((m.flags & 0x10) != 0) != is_static) continue;
    if (m.nparams != param_types.size()) continue;
    bool same = true;
    uint32_t i = 0;
    for (const char* want : param_types) {
      const uint32_t type = param_type(m, i++);
      if (!type || type >= g.n_types || full_name(type) != want) {
        same = false;
        break;
      }
    }
    if (same) return first + k;
  }
  return 0;
}

uintptr_t method_code_typed(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static) {
  const uint32_t m = g_ready ? find_method(t, name, param_types, is_static) : 0;
  return m ? code_of(m) : 0;
}

uintptr_t method_record(uint32_t t, const char* name, int nparams) {
  if (!g_ready || !t || t >= g.n_types) return 0;
  const uint32_t first = first_method_of(t), n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const MethodView m = method_view(first + k);
    if (!m.ok || std::strcmp(m.name, name) != 0) continue;
    if (nparams >= 0 && m.nparams != static_cast<uint32_t>(nparams)) continue;
    return method_at_index(first + k);
  }
  return 0;
}

static bool is_method_record(uintptr_t record) {
  return g_ready && record >= g.methods && record < g.methods + static_cast<uintptr_t>(g.n_methods) * g.method_size &&
         (record - g.methods) % g.method_size == 0;
}

uintptr_t method_record_at(uint32_t index) { return g_ready && index < g.n_methods ? method_at_index(index) : 0; }

// --- method entry hooks (TDB 71) ------------------------------------------------------------
// A method's entry holds its code as an int32 offset from the decoder's base, so
// what replaces it must lie within 2 GB of the game's code - which the mod's DLL
// need not (on Windows both load at random high addresses). Each hook gets a
// jump of its own, `jmp [rip+0]; dq replacement`, in a page allocated near the
// base, and the entry is pointed at that jump. Pages are only ever added.
namespace {

CRITICAL_SECTION g_stub_cs;
bool g_stub_cs_ready = false;
uintptr_t g_stub_page = 0;
size_t g_stub_used = 0;
constexpr size_t kStubPage = 0x10000, kStubSize = 16;

// A free 64 KB region within +-1.5 GB of `near` (VirtualQuery walks: once per page made).
uintptr_t alloc_near(uintptr_t near_to) {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
  const uintptr_t span = 0x60000000ull;
  const uintptr_t lo = near_to > span + 0x10000 ? near_to - span : 0x10000;
  const uintptr_t hi = near_to + span;
  for (int dir = 0; dir < 2; ++dir) {
    // Downwards first (below the image is usually free), then upwards.
    uintptr_t a = dir == 0 ? (near_to & ~(gran - 1)) - gran : (near_to + gran) & ~(gran - 1);
    for (int tries = 0; tries < 20000 && a > lo && a < hi; ++tries) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
      if (mbi.State == MEM_FREE && mbi.RegionSize >= kStubPage) {
        const uintptr_t start = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + gran - 1) & ~(gran - 1);
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(start), kStubPage, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE))
          return reinterpret_cast<uintptr_t>(p);
      }
      const uintptr_t region = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
      a = dir == 0 ? (region > gran ? (region - gran) & ~(gran - 1) : 0)
                   : (region + mbi.RegionSize + gran - 1) & ~(gran - 1);
    }
  }
  return 0;
}

// A jump to `target` reachable from the base by an int32, or 0.
uintptr_t make_stub(uintptr_t target) {
  if (!g_stub_cs_ready) return 0;
  EnterCriticalSection(&g_stub_cs);
  uintptr_t stub = 0;
  if (!g_stub_page || g_stub_used + kStubSize > kStubPage) {
    g_stub_page = alloc_near(g_code_base);
    g_stub_used = 0;
  }
  if (g_stub_page) {
    stub = g_stub_page + g_stub_used;
    g_stub_used += kStubSize;
    uint8_t code[kStubSize] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
    std::memcpy(code + 6, &target, 8);
    code[14] = code[15] = 0xCC;
    std::memcpy(reinterpret_cast<void*>(stub), code, kStubSize);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(stub), kStubSize);
  }
  LeaveCriticalSection(&g_stub_cs);
  return stub;
}

// The jump a stub holds (0: not one of the mod's stubs).
uintptr_t stub_target(uintptr_t stub) {
  if (!stub || !mem::readable(stub, kStubSize)) return 0;
  const auto* b = reinterpret_cast<const uint8_t*>(stub);
  if (b[0] != 0xFF || b[1] != 0x25 || b[2] || b[3] || b[4] || b[5]) return 0;
  return mem::read<uintptr_t>(stub + 6);
}

// Exchange an int32 in place, only while it holds `expected` (as mem::exchange_ptr).
bool exchange_i32(uintptr_t at, int32_t expected, int32_t replacement, DWORD* protect) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (protect) *protect = 0;
  if (!mem::plausible(at) || !VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
    return false;
  if (protect) *protect = mbi.Protect;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const bool as_is = (mbi.Protect & mem::kWritableProtect) != 0;
  DWORD old = 0;
  if (!as_is && !VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_READWRITE, &old)) return false;
  const bool done = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(at), replacement, expected) == expected;
  if (!as_is) VirtualProtect(reinterpret_cast<void*>(at), 4, old, &old);
  return done;
}

}  // namespace

uintptr_t hook_method(uintptr_t record, uintptr_t replacement, unsigned long* protect) {
  if (protect) *protect = 0;
  if (!is_method_record(record) || !replacement || !g_code_base) return 0;
  if (!g_stub_cs_ready) {
    InitializeCriticalSection(&g_stub_cs);
    g_stub_cs_ready = true;
  }
  const uintptr_t slot = record + offsetof(MethodDef, encoded_offset);
  const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
  int32_t enc = 0;
  if (!mem::read_safe(slot, &enc) || !enc) return 0;
  const uintptr_t code = g_code_base + static_cast<intptr_t>(enc);
  if (!text.contains(code)) return 0;
  const uintptr_t stub = make_stub(replacement);
  if (!stub) return 0;
  const intptr_t delta = static_cast<intptr_t>(stub) - static_cast<intptr_t>(g_code_base);
  if (delta < INT32_MIN || delta > INT32_MAX) return 0;
  // Only while the entry still holds the code just read. The database lives in
  // re4.exe's .data: copy-on-write pages, or protected ones made writable for it.
  DWORD prot = 0;
  const bool done = exchange_i32(slot, enc, static_cast<int32_t>(delta), &prot);
  if (protect) *protect = prot;
  return done ? code : 0;
}

bool unhook_method(uintptr_t record, uintptr_t replacement, uintptr_t original) {
  if (!is_method_record(record) || !replacement || !original || !g_code_base) return false;
  const uintptr_t slot = record + offsetof(MethodDef, encoded_offset);
  int32_t enc = 0;
  if (!mem::read_safe(slot, &enc) || !enc) return false;
  const uintptr_t now = g_code_base + static_cast<intptr_t>(enc);
  if (stub_target(now) != replacement) return false;  // only if it still holds ours
  const intptr_t back = static_cast<intptr_t>(original) - static_cast<intptr_t>(g_code_base);
  return back >= INT32_MIN && back <= INT32_MAX && exchange_i32(slot, enc, static_cast<int32_t>(back), nullptr);
}

int method_vt_index(uint32_t method_index) {
  if (!g_ready || method_index >= g.n_methods) return -1;
  const MethodView m = method_view(method_index);
  return m.ok && (m.flags & 0x40) && m.vt_index >= 0 ? m.vt_index : -1;
}

uintptr_t method_code_at(uint32_t method_index) {
  return g_ready && method_index < g.n_methods ? code_of(method_index) : 0;
}

uintptr_t method_target_at(uint32_t method_index) {
  const uintptr_t code = method_code_at(method_index);
  const uintptr_t to = stub_target(code);
  return to ? to : code;
}

uintptr_t vtable_of_object(uintptr_t obj) {
  const uintptr_t info = obj ? mem::read_ptr(obj) : 0;
  return info ? mem::read_ptr(info - 0x10) : 0;
}

uintptr_t vtable_of(uint32_t t) {
  const uintptr_t info = managed_vt(t);
  return info ? mem::read_ptr(info - 0x10) : 0;
}

uintptr_t vtable_slot(uint32_t t, uint32_t method_index, uintptr_t code) {
  const int vt = method_vt_index(method_index);
  const uintptr_t table = vt >= 0 && code ? vtable_of(t) : 0;
  if (!table) return 0;
  const uintptr_t slot = table + static_cast<uintptr_t>(vt) * 8;
  return mem::read_ptr(slot) == code ? slot : 0;
}

int interface_slots(uint32_t t, uintptr_t code, uintptr_t* out, int max, int tables) {
  const uintptr_t info = managed_vt(t);
  if (!info || !code) return 0;
  int n = 0;
  for (int k = 0; k < tables; ++k) {
    const uintptr_t table = mem::read_ptr(info - static_cast<uintptr_t>(3 + k) * 8);
    if (!table || !mem::readable(table, 64 * 8)) continue;
    for (uintptr_t s = table; s < table + 64 * 8 && n < max; s += 8)
      if (mem::read_ptr(s) == code) out[n++] = s;
  }
  return n;
}

bool swap_slot(uintptr_t slot, uintptr_t expected, uintptr_t replacement) {
  return slot && expected && replacement && mem::exchange_ptr(slot, expected, replacement);
}

int classes_sharing_vtable(uint32_t t) {
  const uintptr_t table = vtable_of(t);
  if (!table) return -1;
  int n = 0;
  for (uint32_t i = 1; i < g.n_types; ++i)
    if (i != t && td_managed_vt(i) && mem::read_ptr(td_managed_vt(i) - 0x10) == table) ++n;
  return n;
}

int globals_pointing_at(uintptr_t record) {
  const mem::Range data = mem::section(GetModuleHandleA(nullptr), ".data");
  if (!record || data.empty() || !mem::readable(data.begin, data.size())) return -1;
  int n = 0;
  for (uintptr_t a = (data.begin + 7) & ~static_cast<uintptr_t>(7); a + 8 <= data.end; a += 8)
    if (*reinterpret_cast<const uintptr_t*>(a) == record) ++n;
  return n;
}

std::string method_at(uintptr_t record) {
  if (!is_method_record(record)) return {};
  const MethodView m = method_view(static_cast<uint32_t>((record - g.methods) / g.method_size));
  return (m.declaring < g.n_types ? full_name(m.declaring) : std::string("?")) + "." + (m.ok ? m.name : "?");
}

bool dump_methods(const char* path) {
  if (!g_ready) return false;
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const mem::Range image = mem::module_range(GetModuleHandleA(nullptr));
  const uint32_t header[4] = {0x4D344552 /* "RE4M" */, 1, g.n_methods, 0};
  std::fwrite(header, sizeof(header), 1, f);
  uint32_t with_code = 0;
  std::vector<uint32_t> rvas(g.n_methods, 0);
  for (uint32_t i = 0; i < g.n_methods; ++i) {
    const uintptr_t code = code_of(i);
    if (code && image.contains(code)) {
      rvas[i] = static_cast<uint32_t>(code - base);
      ++with_code;
    }
  }
  std::fwrite(rvas.data(), 4, rvas.size(), f);
  std::fclose(f);
  logf("re: wrote %s - %u of %u methods have code in the image", path, with_code, g.n_methods);
  return true;
}

}  // namespace re4cc::re
