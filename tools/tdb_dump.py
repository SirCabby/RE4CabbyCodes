#!/usr/bin/env python3
"""Read RE Engine's type database straight out of re4.exe on disk - TDB v71 (Resident
Evil 4's Steam build 22377325).

    python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --dump tdb.txt       # every type, one block each
    python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --type chainsaw.InventoryManager
    python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --grep 'HitPoint'   # type/field/method names matching a regex
    python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --enum chainsaw.GameDifficulty
    python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin   # for test_discover

The executable carries its whole reflection database in .data, uninitialised: the
header's array pointers are offsets from the header itself (the VM turns them into
pointers when it starts). Layout per REFramework's tdb71 structs (shared/sdk/RETypeDB.hpp,
RETypeDefinition.hpp: RETypeDefVersion71), confirmed here by every array ending exactly
where the next begins. Unlike TDB 70 (RE2, RE3), a method's code is no pointer the VM
fills in but an `encoded_offset` from a fixed base in .text, read out of the engine's
decoder (`test ecx,ecx; jne; xor eax,eax; ret; movsxd rax,ecx; lea rcx,[base]; add rax,rcx;
ret`) - so every method's code RVA is known from the file alone. Pure stdlib.
"""
import argparse, mmap, re, struct, sys

# via::clr field / method flags (the .NET attribute values)
F_STATIC, F_LITERAL = 0x10, 0x40
M_STATIC, M_VIRTUAL, M_ABSTRACT = 0x10, 0x40, 0x400


class PE:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        m = self.m
        pe = struct.unpack_from("<I", m, 0x3C)[0]
        nsec = struct.unpack_from("<H", m, pe + 6)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<Q", m, opt + 24)[0]
        opt_sz = struct.unpack_from("<H", m, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + opt_sz + 40 * i
            name = m[o:o + 8].rstrip(b"\0").decode("latin1")
            vs, va, rs, ro = struct.unpack_from("<IIII", m, o + 8)
            self.secs.append((name, va, vs, ro, rs))

    def section(self, name):
        for s in self.secs:
            if s[0] == name:
                return s
        return None

    def rva_to_off(self, rva):
        for _, va, vs, ro, rs in self.secs:
            if va <= rva < va + max(vs, rs):
                return ro + rva - va if rva - va < rs else None
        return None

    def off_to_rva(self, off):
        for _, va, vs, ro, rs in self.secs:
            if ro <= off < ro + rs:
                return va + off - ro
        return None


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# The engine's decoder of a method's encoded_offset (REFramework's get_encoded_pointer pattern).
DECODER = re.compile(rb"\x85\xC9\x75\x03\x33\xC0\xC3\x48\x63\xC1\x48\x8D\x0D(....)\x48\x03\xC1\xC3", re.S)


class TDB:
    def __init__(self, pe, off=None):
        self.pe = pe
        m = pe.m
        if off is None:
            off = self.find(pe)
        self.off = off
        self.rva = pe.off_to_rva(off)
        h = struct.unpack_from("<22I", m, off)
        (magic, self.version, self.initialized, self.n_types, self.n_methods, self.n_fields, self.n_type_impl,
         self.n_field_impl, self.n_method_impl, self.n_prop_impl, self.n_props, self.n_events, self.n_params,
         self.n_attrs, self.n_init_data, _attrs2, self.n_intern, self.n_modules, self.dev_entry, self.app_entry,
         self.n_strings, self.n_bytes) = h
        assert magic == 0x424454, "no TDB magic"
        ptrs = struct.unpack_from("<18Q", m, off + 0x58)
        (self.p_modules, self.p_types, self.p_types_impl, self.p_methods, self.p_methods_impl, self.p_fields,
         self.p_fields_impl, self.p_props, self.p_props_impl, self.p_events, self.p_params, self.p_attrs,
         self.p_init_data, _unk, self.p_attrs2, self.p_strings, self.p_bytes, self.p_intern) = ptrs
        if self.initialized:
            raise SystemExit("this TDB is initialised (absolute pointers) - read it from the exe on disk, not a dump")
        # names are masked by the string pool's size rounded up to a power of two (REFramework's
        # get_string_pool_bitmask): the high bits of a name offset carry other data
        b = 1
        while b < self.n_strings:
            b <<= 1
        self.string_mask = b - 1
        self.code_base = self.find_code_base()
        self.codes = None   # method index -> code RVA (link_codes)
        self.vtables = {}   # type index -> (vtable RVA, slots), from the link records
        self._names = {}

    @staticmethod
    def find(pe):
        _, va, vs, ro, rs = pe.section(".data")
        m = pe.m
        pos = m.find(b"TDB\0", ro, ro + rs)
        while pos != -1:
            ver = struct.unpack_from("<I", m, pos + 4)[0]
            if ver == 71 and (pos - ro) % 8 == 0 and struct.unpack_from("<Q", m, pos + 0x60)[0] == 0x300:
                return pos
            pos = m.find(b"TDB\0", pos + 1, ro + rs)
        raise SystemExit("TDB v71 not found in .data")

    def find_code_base(self):
        _, va, vs, ro, rs = self.pe.section(".text")
        found = [mo for mo in DECODER.finditer(self.pe.m, ro, ro + rs)]
        if len(found) != 1:
            print("warning: %d method-offset decoders in .text - code RVAs unknown" % len(found), file=sys.stderr)
            return 0
        rva = self.pe.off_to_rva(found[0].start())
        self.decoder_rva = rva
        return rva + 17 + struct.unpack("<i", found[0].group(1))[0]

    # --- raw access -------------------------------------------------------------------
    def string(self, off):
        off &= self.string_mask
        if off >= self.n_strings:
            return "?"
        a = self.off + self.p_strings + off
        e = self.pe.m.find(b"\0", a)
        return self.pe.m[a:e].decode("utf-8", "replace")

    def byte_at(self, off):
        return self.off + self.p_bytes + off

    def typedef(self, i):
        """RETypeDefVersion71 (0x48)."""
        o = self.off + self.p_types + i * 0x48
        a, b, flags, size, fqn, crc, c, d, e, g = struct.unpack_from("<QQIIIIQIIq", self.pe.m, o)
        return dict(index=a & 0x7FFFF, parent=(a >> 19) & 0x7FFFF, declaring=(a >> 38) & 0x7FFFF,
                    underlying=(a >> 57) & 0x7F, array=b & 0x7FFFF, element=(b >> 19) & 0x7FFFF,
                    impl=(b >> 38) & 0x3FFFF, system=(b >> 56) & 0x7F, flags=flags, size=size, fqn=fqn, crc=crc,
                    ctor=c & 0x3FFFFF, member_method=(c >> 22) & 0x3FFFFF, member_field=(c >> 44) & 0x7FFFF,
                    num_prop=d & 0xFFF, member_prop=(d >> 12) & 0x7FFFF, obj_type=(e >> 26) & 7,
                    generics=sext((g >> 26) & 0x3FFFFFF, 26), interfaces=sext((g >> 52) & 0xFFF, 12))

    def typeimpl(self, i):
        """RETypeImpl (0x30, v71)."""
        o = self.off + self.p_types_impl + i * 0x30
        name, ns, fsz, sfsz, w, nmeth, nnvt, iface = struct.unpack_from("<iiiiQHhh", self.pe.m, o)
        return dict(name=name, ns=ns, field_size=fsz, static_size=sfsz, num_fields=(w >> 33) & 0xFFFFFF,
                    num_methods=nmeth, num_native_vt=nnvt, interface=iface)

    def method(self, i):
        """REMethodDefinition (0xC, v71): the code is encoded_offset from the decoder's base."""
        o = self.off + self.p_methods + i * 12
        a, b, enc = struct.unpack_from("<IIi", self.pe.m, o)
        return dict(declaring=a & 0x7FFFF, impl=b & 0x7FFFF, params=((b >> 19) << 13) | (a >> 19), enc=enc)

    def code_rva(self, i):
        """The method's code RVA as the VM will link it (link records; encoded_offset is 0 on disk)."""
        if self.codes is None:
            self.codes = self.link_codes()
        return self.codes.get(i, 0)

    # The VM links method code at start-up from one record per class in .data - the class's
    # type index, its vtable and the array of its methods' code in database order - and
    # writes each method's encoded_offset from it (0 on disk). The same 0x28-byte record as
    # TDB 70's: {u32 type, u32 ?, u32 vt slots, u32 methods, u64 vtable, u64 code, u64 global}.
    LINK_RECORD = 0x28

    def link_codes(self):
        m, base = self.pe.m, self.pe.image_base
        _, dva, dvs, dro, drs = self.pe.section(".data")
        _, tva, tvs, _, _ = self.pe.section(".text")
        counts, codes = {}, {}
        o, end = dro, dro + drs - self.LINK_RECORD
        top = base + 0x20000000
        while o < end:
            ty, _f, nvt, n, vt, code = struct.unpack_from("<IIIIQQ", m, o)
            if 0 < ty < self.n_types and 0 < n < 5000 and nvt < 5000 and base < code < top and \
                    (vt == 0 or base < vt < top):
                if ty not in counts:
                    t = self.typedef(ty)
                    counts[ty] = self.typeimpl(t["impl"])["num_methods"] if t["member_method"] else 0
                if n == counts[ty]:
                    arr = self.pe.rva_to_off(code - base)
                    first = self.typedef(ty)["member_method"]
                    if arr is not None:
                        self.vtables[ty] = (vt - base if vt else 0, nvt)
                        for k, v in enumerate(struct.unpack_from("<%dQ" % n, m, arr)):
                            if v and tva <= v - base < tva + tvs:
                                codes[first + k] = v - base
                    o += self.LINK_RECORD
                    continue
            o += 8
        return codes

    def method_impl(self, i):
        o = self.off + self.p_methods_impl + i * 12
        attr, vtidx, flags, iflags, name = struct.unpack_from("<HhHHI", self.pe.m, o)
        return dict(vt_index=vtidx, flags=flags, impl_flags=iflags, name=name)

    def field(self, i):
        o = self.off + self.p_fields + i * 8
        a = struct.unpack_from("<Q", self.pe.m, o)[0]
        return dict(declaring=a & 0x7FFFF, impl=(a >> 19) & 0x7FFFF, type=(a >> 38) & 0x7FFFF, init_hi=(a >> 57) & 0x3F)

    def field_impl(self, i):
        o = self.off + self.p_fields_impl + i * 12
        attr, fl, w1, w2 = struct.unpack_from("<HHII", self.pe.m, o)
        return dict(flags=fl >> 1, offset=w1 & 0x3FFFFFF, init_lo=w1 >> 26, name=w2 & 0xFFFFFFF, init_mid=w2 >> 28)

    def param(self, i):
        o = self.off + self.p_params + i * 12
        attr, init, w1, w2 = struct.unpack_from("<HHII", self.pe.m, o)
        return dict(name=w1 & 0x3FFFFFFF, modifier=w1 >> 30, type=w2 & 0x7FFFF, flags=w2 >> 19)

    def prop(self, i):
        o = self.off + self.p_props + i * 8
        a = struct.unpack_from("<Q", self.pe.m, o)[0]
        return dict(impl=a & 0xFFFFF, getter=(a >> 20) & 0x3FFFFF, setter=(a >> 42) & 0x3FFFFF)

    def prop_impl(self, i):
        o = self.off + self.p_props_impl + i * 8
        flags, attr, name = struct.unpack_from("<HHi", self.pe.m, o)
        return dict(flags=flags, name=name)

    def init_data(self, index):
        """Where a literal's value lives (byte pool, or the string pool for a negative offset)."""
        if index >= self.n_init_data:
            return None
        off = struct.unpack_from("<i", self.pe.m, self.off + self.p_init_data + index * 4)[0]
        if off < 0:
            return ("str", self.string(-off))
        return ("bytes", self.byte_at(off))

    def type_sizes(self, i):
        ti = self.typeimpl(self.typedef(i)["impl"])
        return ti["field_size"], ti["static_size"]

    # --- names --------------------------------------------------------------------------
    def short_name(self, i):
        t = self.typedef(i)
        ti = self.typeimpl(t["impl"])
        return self.string(ti["ns"]), self.string(ti["name"])

    def name(self, i, depth=0):
        if i in self._names:
            return self._names[i]
        if i == 0 or i >= self.n_types or depth > 16:
            return "?"
        t = self.typedef(i)
        ns, nm = self.short_name(i)
        if not nm and t["element"] and t["element"] != i and t["element"] < self.n_types:
            full = self.name(t["element"], depth + 1) + "[]"   # an array: named by reflection in the engine
            self._names[i] = full
            return full
        if t["declaring"] and t["declaring"] != i and t["declaring"] < self.n_types:
            full = self.name(t["declaring"], depth + 1) + "." + nm
        else:
            full = (ns + "." if ns else "") + nm
        g = self.generic_args(i)
        if g:
            full += "<" + ",".join(self.name(x, depth + 1) for x in g) + ">"
        self._names[i] = full
        return full

    def generic_args(self, i):
        t = self.typedef(i)
        if t["generics"] <= 0 or t["generics"] >= self.n_bytes:
            return []
        w = struct.unpack_from("<I", self.pe.m, self.byte_at(t["generics"]))[0]
        definition, num = w & 0x7FFFF, w >> 19
        if definition == i or num == 0 or num > 16:
            return []
        return list(struct.unpack_from("<%dI" % num, self.pe.m, self.byte_at(t["generics"]) + 4))

    # --- members --------------------------------------------------------------------------
    def fields_of(self, i):
        t = self.typedef(i)
        n = self.typeimpl(t["impl"])["num_fields"]
        if not t["member_field"]:
            return []
        out = []
        for k in range(n):
            f = self.field(t["member_field"] + k)
            fi = self.field_impl(f["impl"])
            out.append(dict(index=t["member_field"] + k, name=self.string(fi["name"]), type=f["type"],
                            offset=fi["offset"], flags=fi["flags"],
                            init=fi["init_lo"] | (fi["init_mid"] << 6) | (f["init_hi"] << 10)))
        return out

    def methods_of(self, i):
        t = self.typedef(i)
        n = self.typeimpl(t["impl"])["num_methods"]
        if not t["member_method"]:
            return []
        out = []
        for k in range(n):
            idx = t["member_method"] + k
            md = self.method(idx)
            mi = self.method_impl(md["impl"])
            ret, params = self.signature(md["params"])
            out.append(dict(index=idx, name=self.string(mi["name"]), code=self.code_rva(idx), flags=mi["flags"],
                            impl_flags=mi["impl_flags"], vt_index=mi["vt_index"], ret=ret, params=params))
        return out

    def signature(self, pl):
        """(return type, [(type, name)]) from a ParamList in the byte pool."""
        if pl >= self.n_bytes:
            return 0, []
        num, invoke, ret = struct.unpack_from("<HHI", self.pe.m, self.byte_at(pl))
        if num > 64:
            return 0, []
        ids = struct.unpack_from("<%dI" % num, self.pe.m, self.byte_at(pl) + 8) if num else ()
        rt = self.param(ret)["type"] if ret < self.n_params else 0
        return rt, [(self.param(p)["type"], self.string(self.param(p)["name"])) for p in ids if p < self.n_params]

    def props_of(self, i):
        t = self.typedef(i)
        out = []
        for k in range(t["num_prop"]):
            p = self.prop(t["member_prop"] + k)
            pi = self.prop_impl(p["impl"])
            out.append(dict(name=self.string(pi["name"]), getter=p["getter"], setter=p["setter"]))
        return out

    SIZES = {"System.Int8": 1, "System.UInt8": 1, "System.Byte": 1, "System.SByte": 1, "System.Int16": 2,
             "System.UInt16": 2, "System.Int64": 8, "System.UInt64": 8, "System.Boolean": 1, "System.Char": 2}

    def literal_value(self, f):
        d = self.init_data(f["init"])
        if not d:
            return None
        if d[0] == "str":
            return repr(d[1])
        size = self.SIZES.get(self.name(f["type"]), 4)
        # an enum's literals carry the enum type: use its underlying value field
        ft = self.typedef(f["type"]) if f["type"] < self.n_types else None
        if ft and self.name(ft["parent"]) == "System.Enum":
            for g in self.fields_of(f["type"]):
                if not g["flags"] & F_STATIC:
                    size = self.SIZES.get(self.name(g["type"]), 4)
                    if size == 1 and self.name(g["type"]) == "System.Boolean":
                        size = 1
                    break
        if self.name(f["type"]) == "System.Single":
            return struct.unpack_from("<f", self.pe.m, d[1])[0]
        fmt = {1: "<b", 2: "<h", 4: "<i", 8: "<q"}[size]
        return struct.unpack_from(fmt, self.pe.m, d[1])[0]

    def all_names(self):
        if not hasattr(self, "_by_name"):
            self._by_name = {}
            for i in range(1, self.n_types):
                self._by_name.setdefault(self.name(i), i)
        return self._by_name

    def find_type(self, name):
        return self.all_names().get(name)

    # --- printing ---------------------------------------------------------------------------
    def describe(self, i, inherited=True, out=sys.stdout):
        t = self.typedef(i)
        chain = []
        p = t["parent"]
        while p and len(chain) < 24:
            chain.append(self.name(p))
            p = self.typedef(p)["parent"]
        field_size, static_size = self.type_sizes(i)
        print("type %d %s  size=0x%X fields=0x%X static=0x%X objtype=%d flags=0x%X fqn=%08X" % (
            i, self.name(i), t["size"], field_size, static_size, t["obj_type"], t["flags"], t["fqn"]), file=out)
        if chain:
            print("  : " + " : ".join(chain), file=out)
        levels = [i] + ([x for x in self._parents(i)] if inherited else [])
        for lv in levels:
            fs = self.fields_of(lv)
            if not fs:
                continue
            if lv != i:
                print("  -- fields of %s" % self.name(lv), file=out)
            for f in fs:
                tag = ""
                if f["flags"] & F_LITERAL:
                    tag = " = %s" % self.literal_value(f)
                elif f["flags"] & F_STATIC:
                    tag = " [static]"
                print("  field +0x%-5X %-50s %s%s" % (f["offset"], self.name(f["type"]), f["name"], tag), file=out)
        for p_ in self.props_of(i):
            print("  prop  %s (get %d set %d)" % (p_["name"], p_["getter"], p_["setter"]), file=out)
        for mth in self.methods_of(i):
            args = ", ".join("%s %s" % (self.name(a), n) for a, n in mth["params"])
            kind = ("static " if mth["flags"] & M_STATIC else "") + \
                   ("virtual[%d] " % mth["vt_index"] if mth["flags"] & M_VIRTUAL else "")
            code = mth["code"]
            print("  meth  [%d] %s%s %s(%s)  @ %s" % (mth["index"], kind, self.name(mth["ret"]) if mth["ret"] else "void",
                                                     mth["name"], args, ("0x%X" % code) if code else "-"), file=out)

    def _parents(self, i):
        p = self.typedef(i)["parent"]
        while p and p < self.n_types:
            yield p
            p = self.typedef(p)["parent"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dump", help="write every type to this file")
    ap.add_argument("--type", action="append", default=[], help="describe a type (full name), inherited fields included")
    ap.add_argument("--grep", help="regex over type, field and method names")
    ap.add_argument("--enum", action="append", default=[], help="print an enum's values")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--write-methods", help="write every method's code RVA, one u32 per method index (tests)")
    a = ap.parse_args()
    pe = PE(a.exe)
    tdb = TDB(pe)
    if a.write_methods:
        rvas = [tdb.code_rva(i) for i in range(tdb.n_methods)]
        with open(a.write_methods, "wb") as out:
            out.write(struct.pack("<4I", 0x4D344552, 1, tdb.n_methods, 0))
            out.write(struct.pack("<%dI" % len(rvas), *rvas))
        print("wrote %s (%d of %d methods with code)" % (a.write_methods, sum(1 for r in rvas if r), tdb.n_methods))
    if a.summary or not (a.dump or a.type or a.grep or a.enum or a.write_methods):
        print("TDB v%d at file 0x%X (RVA 0x%X): %d types, %d methods, %d fields, %d params, strings 0x%X, bytes 0x%X" % (
            tdb.version, tdb.off, tdb.rva, tdb.n_types, tdb.n_methods, tdb.n_fields, tdb.n_params, tdb.n_strings, tdb.n_bytes))
        print("method code base RVA 0x%X (decoder at RVA 0x%X)" % (tdb.code_base, getattr(tdb, "decoder_rva", 0)))
        bad = sum(1 for i in range(1, tdb.n_types) if tdb.typedef(i)["index"] != i)
        print("typedef index self-check: %d mismatches" % bad)
    for name in a.type:
        i = tdb.find_type(name)
        if i is None:
            print("no type %s" % name)
            continue
        tdb.describe(i)
    for name in a.enum:
        i = tdb.find_type(name)
        if i is None:
            print("no type %s" % name)
            continue
        for f in tdb.fields_of(i):
            if f["flags"] & F_LITERAL:
                print("%s = %s" % (f["name"], tdb.literal_value(f)))
    if a.grep:
        rx = re.compile(a.grep)
        for i in range(1, tdb.n_types):
            n = tdb.name(i)
            if rx.search(n):
                print("TYPE  %s" % n)
            for f in tdb.fields_of(i):
                if rx.search(f["name"]):
                    print("FIELD %s.%s  (+0x%X %s%s)" % (n, f["name"], f["offset"], tdb.name(f["type"]),
                                                          " static" if f["flags"] & F_STATIC else ""))
            for mth in tdb.methods_of(i):
                if rx.search(mth["name"]):
                    code = mth["code"]
                    print("METH  %s.%s  [%d]%s" % (n, mth["name"], mth["index"], "  @ 0x%X" % code if code else ""))
    if a.dump:
        with open(a.dump, "w") as out:
            for i in range(1, tdb.n_types):
                tdb.describe(i, inherited=False, out=out)
        print("wrote %s" % a.dump)


if __name__ == "__main__":
    main()
