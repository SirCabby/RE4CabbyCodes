#!/usr/bin/env python3
"""Read the RSZ block of an RE Engine user data file (.user) with the type database.

    python3 tools/rsz.py --game "$GAME_DIR" --type offline.RecordListUserData   # every file holding one, parsed

An RSZ block is a table of instances, each one class's serialised fields in the
database's order (base classes first), every field aligned to its type: values in
place, references as the index of another instance, strings as a length and
UTF-16, arrays and lists as a count and their elements. The reader takes the
classes from the type database (tools/tdb_dump.py), so it reads any user file
whose classes hold the kinds of field below; it raises on anything else rather
than guess. Pure stdlib.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb_dump import PE, TDB, F_LITERAL, F_STATIC  # noqa: E402
from pak_msg import open_paks  # noqa: E402

# Value types as RSZ stores them: (size, alignment, struct format).
PRIM = {"System.Int8": (1, 1, "b"), "System.UInt8": (1, 1, "B"), "System.Byte": (1, 1, "B"), "System.SByte": (1, 1, "b"),
        "System.Boolean": (1, 1, "?"), "System.Int16": (2, 2, "h"), "System.UInt16": (2, 2, "H"),
        "System.Int32": (4, 4, "i"), "System.UInt32": (4, 4, "I"), "System.Single": (4, 4, "f"),
        "System.Int64": (8, 8, "q"), "System.UInt64": (8, 8, "Q"), "System.Double": (8, 8, "d"),
        "via.vec2": (8, 4, "2f"), "via.vec3": (16, 16, "4f"), "via.vec4": (16, 16, "4f"),
        "via.Quaternion": (16, 16, "4f"), "via.Float2": (8, 4, "2f"), "via.Float3": (12, 4, "3f"),
        "via.Float4": (16, 4, "4f"), "via.Color": (4, 4, "I"), "via.Int2": (8, 4, "2i"), "via.Int3": (12, 4, "3i"),
        "via.Uint2": (8, 4, "2I"), "via.Range": (8, 4, "2f"), "via.RangeI": (8, 4, "2i"), "via.mat4": (64, 16, "16f")}


def rsz_at(data):
    """Where a user data file's RSZ block starts: the USR header's data offset (+0x20), or
    wherever the magic is in the first 0x200 bytes when that one points elsewhere."""
    r = struct.unpack_from("<Q", data, 0x20)[0] if len(data) >= 0x28 else -1
    if 0 <= r < len(data) - 4 and data[r:r + 4] == b"RSZ\0":
        return r
    return data.find(b"RSZ\0", 0, 0x200)


def instance_types(data):
    """The type ids of an RSZ block's instances (0 for the null instance), or [] when it has none."""
    r = rsz_at(data)
    if r < 0:
        return []
    try:
        n = struct.unpack_from("<i", data, r + 0xC)[0]
        ioff = struct.unpack_from("<Q", data, r + 0x18)[0]
        return [struct.unpack_from("<I", data, r + ioff + 8 * i)[0] for i in range(n)]
    except struct.error:
        return []


class Reader:
    def __init__(self, tdb):
        self.tdb = tdb
        self.by_fqn = {}
        for i in range(1, tdb.n_types):
            self.by_fqn.setdefault(tdb.typedef(i)["fqn"], i)

    def fqn(self, name):
        t = self.tdb.find_type(name)
        return self.tdb.typedef(t)["fqn"] if t else None

    def fields(self, t):
        chain, x = [], t
        while x and x < self.tdb.n_types and len(chain) < 32:
            chain.append(x)
            x = self.tdb.typedef(x)["parent"]
        out = []
        for c in reversed(chain):
            out += [f for f in self.tdb.fields_of(c) if not f["flags"] & (F_STATIC | F_LITERAL)]
        return out

    def _underlying(self, t):
        td = self.tdb.typedef(t)
        if td["obj_type"] == 5 and self.tdb.name(td["parent"]) == "System.Enum":
            for g in self.tdb.fields_of(t):
                if not g["flags"] & F_STATIC:
                    return self.tdb.name(g["type"])
            return "System.Int32"
        return None

    def _value(self, data, o, t):
        tn = self._underlying(t) or self.tdb.name(t)
        if tn in PRIM:
            size, align, fmt = PRIM[tn]
            o = (o + align - 1) & ~(align - 1)
            v = struct.unpack_from("<" + fmt, data, o)
            return o + size, (v[0] if len(v) == 1 else v)
        if tn == "System.Guid":
            o = (o + 7) & ~7
            return o + 16, bytes(data[o:o + 16])
        if tn == "System.String":
            o = (o + 3) & ~3
            n = struct.unpack_from("<I", data, o)[0]
            o += 4
            return o + 2 * n, data[o:o + 2 * n].decode("utf-16-le", "replace").rstrip("\0")
        td = self.tdb.typedef(t)
        if td["obj_type"] == 5:  # another value type: its fields in place
            vals = {}
            for f in self.fields(t):
                o, vals[f["name"]] = self._field(data, o, f["type"])
            return o, vals
        o = (o + 3) & ~3  # a reference: the index of an instance
        return o + 4, ("#", struct.unpack_from("<I", data, o)[0])

    def _field(self, data, o, t):
        tn = self.tdb.name(t)
        td = self.tdb.typedef(t)
        is_array = (not tn or tn.endswith("[]")) and td["element"]
        if is_array or tn.startswith("System.Collections.Generic.List`1<"):
            et = td["element"] if is_array else (self.tdb.generic_args(t) or [0])[0]
            if not et:
                raise ValueError("%s: no element type" % tn)
            o = (o + 3) & ~3
            n = struct.unpack_from("<I", data, o)[0]
            o += 4
            out = []
            for _ in range(n):
                o, v = self._value(data, o, et)
                out.append(v)
            return o, out
        return self._value(data, o, t)

    def parse(self, data):
        """[(class name, {field: value})] by instance index (0: None), the bytes read and the block's size."""
        r = rsz_at(data)
        if r < 0:
            raise ValueError("no RSZ block")
        magic, _ver, _nobj, ninst, nuser, _res, ioff, doff, uoff = struct.unpack_from("<4sIiiiiQQQ", data, r)
        types = [struct.unpack_from("<I", data, r + ioff + 8 * i)[0] for i in range(ninst)]
        # Another user data file referenced by this one (its table: the instance,
        # its type, its path): an instance with no fields in this block.
        external = {struct.unpack_from("<I", data, r + uoff + 16 * k)[0] for k in range(nuser)}
        out = [None]
        o = r + doff
        for i in range(1, ninst):
            t = self.by_fqn.get(types[i])
            if t is None:
                raise ValueError("instance %d: type id %08X not in the database" % (i, types[i]))
            vals = {}
            if i not in external:
                for f in self.fields(t):
                    o, vals[f["name"]] = self._field(data, o, f["type"])
            out.append((self.tdb.name(t), vals))
        return out, o - r, len(data) - r


def files_holding(table, fqn, max_size=8 << 20):
    """Every user data file in the archives with an instance of the type id: [(key, data)]."""
    out = []
    for key, (pak, e) in table.items():
        if e[4] > max_size:
            continue
        try:
            data = pak.read(e)
        except Exception:
            continue
        if data[:4] == b"USR\0" and fqn in instance_types(data):
            out.append((key, data))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--type", required=True)
    a = ap.parse_args()
    tdb = TDB(PE(os.path.join(a.game, "re4.exe")))
    reader = Reader(tdb)
    fqn = reader.fqn(a.type)
    if fqn is None:
        raise SystemExit("no type %s" % a.type)
    _, table = open_paks(a.game)
    for key, data in files_holding(table, fqn):
        try:
            insts, used, size = reader.parse(data)
        except (ValueError, struct.error) as ex:
            print("== %s: not parsed: %s" % (key, ex))
            continue
        print("== %s: %d instances, %d of %d bytes" % (key, len(insts) - 1, used, size))
        for i, inst in enumerate(insts[1:], 1):
            print("  #%d %s %s" % (i, inst[0], inst[1]))


if __name__ == "__main__":
    main()
