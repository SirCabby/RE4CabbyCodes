#!/usr/bin/env python3
"""Evaluate one of the game's compiled switch functions for a key, offline.

    python3 -I tools/switch_emu.py --exe "$GAME_DIR/re4.exe" --rva 0x2345680 112800000

The game answers some questions about an item id only in code: chainsaw.ItemIDDefine's
getItemType, getWeaponID and the weapon's category are static methods compiled into
chains of compares and jump tables - pure functions of their one argument (edx; the
VM context in rcx is never read). This walks such a function instruction by
instruction over objdump's disassembly of re4.exe on disk: compares, conditional
and direct jumps, jump tables (`movsxd`/`mov` from a table in the image, `add`,
`jmp reg`), register moves, `lea`, small arithmetic, setcc, `ret`. Anything else
raises - an answer is never a guess. tools/gen_items.py uses it.
"""
import argparse, re, struct, subprocess

BASE = 0x140000000


class Image:
    """re4.exe on disk: its sections, for the jump tables' reads."""

    def __init__(self, exe):
        self.exe = exe
        self.d = open(exe, "rb").read()
        pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.d, pe + 6)[0]
        opt = pe + 24
        osz = struct.unpack_from("<H", self.d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + osz + 40 * i
            vs, va, rs, ro = struct.unpack_from("<IIII", self.d, o + 8)
            self.secs.append((va, max(vs, rs), ro, rs))

    def read(self, va, n):
        rva = va - BASE
        for sva, size, ro, rs in self.secs:
            if sva <= rva < sva + size and rva - sva + n <= rs:
                return self.d[ro + rva - sva: ro + rva - sva + n]
        raise ValueError("read outside the image at 0x%X" % va)


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


class Emu:
    REG32 = {"eax": "rax", "ecx": "rcx", "edx": "rdx", "r8d": "r8", "r9d": "r9", "r10d": "r10", "r11d": "r11"}

    def __init__(self, exe):
        self.img = Image(exe)
        self.cache = {}

    def insn(self, addr):
        if addr in self.cache:
            return self.cache[addr]
        out = subprocess.run(["objdump", "-d", "-M", "intel", "--no-show-raw-insn", "--start-address=0x%x" % addr,
                              "--stop-address=0x%x" % (addr + 0x200), self.img.exe],
                             capture_output=True, text=True).stdout
        lst = []
        for ln in out.splitlines():
            m = re.match(r"^\s+([0-9a-f]+):\s+(.*)$", ln)
            if m:
                lst.append((int(m.group(1), 16), m.group(2).strip()))
        # objdump runs on into data past a function's end: stop at what is not code
        for i, (a, t) in enumerate(lst):
            if t.startswith((".byte", "(bad)")):
                lst = lst[:i]
                break
        for i in range(len(lst) - 1):
            self.cache.setdefault(lst[i][0], (lst[i][1], lst[i + 1][0]))
        if addr not in self.cache:
            raise ValueError("no instruction at 0x%X" % addr)
        return self.cache[addr]

    def run(self, rva, key, steps=5000):
        regs = {"rax": 0, "rcx": 0, "rdx": s32(key), "r8": 0, "r9": 0, "r10": 0, "r11": 0}
        flags = (0, 0)
        pc = BASE + rva

        def reg(name):
            if name in regs:
                return regs[name]
            if name in self.REG32:
                return s32(regs[self.REG32[name]])
            if name == "al":
                return regs["rax"] & 0xFF
            raise ValueError("register %s" % name)

        def setreg(name, v):
            if name == "al":
                regs["rax"] = (regs["rax"] & ~0xFF) | (v & 0xFF)
            elif name in regs:
                regs[name] = v
            elif name in self.REG32:  # a 32-bit write clears the upper half
                regs[self.REG32[name]] = v & 0xFFFFFFFF if v >= 0 else s32(v) & 0xFFFFFFFF
                regs[self.REG32[name]] = s32(regs[self.REG32[name]])
            else:
                raise ValueError("register %s" % name)

        def val(x, t):
            x = x.strip()
            if re.match(r"^-?(0x[0-9a-f]+|\d+)$", x):
                return int(x, 0)
            m = re.match(r"^(DWORD|BYTE|QWORD|WORD) PTR \[(.+)\]$", x)
            if m:
                return mem(m.group(1), m.group(2), t)
            return reg(x)

        def addr_of(expr, t):
            if "rip" in expr:
                m = re.search(r"#\s*(?:0x)?([0-9a-f]+)", t)
                return int(m.group(1), 16)
            total = 0
            for term in re.findall(r"[+-]?[^+-]+", expr.replace(" ", "")):
                sign = -1 if term.startswith("-") else 1
                term = term.lstrip("+-")
                if "*" in term:
                    r, sc = term.split("*")
                    total += sign * reg(r) * int(sc)
                elif re.match(r"^(0x[0-9a-f]+|\d+)$", term):
                    total += sign * int(term, 0)
                else:
                    total += sign * reg(term)
            return total

        def mem(size, expr, t):
            a = addr_of(expr, t)
            n = {"BYTE": 1, "WORD": 2, "DWORD": 4, "QWORD": 8}[size]
            raw = self.img.read(a, n)
            return int.from_bytes(raw, "little", signed=False)

        for _ in range(steps):
            t, nxt = self.insn(pc)
            t = re.sub(r"\s+", " ", t)
            op, _, args = t.partition(" ")
            args = args.split("#")[0].strip() if "rip" not in args else args
            a = [x.strip() for x in args.split(",", 1)] if args else []
            if op == "cmp":
                flags = (s32(val(a[0], t)), s32(val(a[1], t)))
            elif op == "test":
                flags = (s32(val(a[0], t) & val(a[1], t)), 0)
            elif op in ("je", "jne", "jle", "jl", "jg", "jge", "ja", "jae", "jb", "jbe", "js", "jns"):
                x, y = flags
                ux, uy = x & 0xFFFFFFFF, y & 0xFFFFFFFF
                take = {"je": x == y, "jne": x != y, "jle": x <= y, "jl": x < y, "jg": x > y, "jge": x >= y,
                        "ja": ux > uy, "jae": ux >= uy, "jb": ux < uy, "jbe": ux <= uy,
                        "js": s32(x - y) < 0, "jns": s32(x - y) >= 0}[op]
                if take:
                    pc = int(args.split(" ")[0], 16)
                    continue
            elif op == "jmp":
                target = args.split(" ")[0]
                pc = int(target, 16) if re.match(r"^(0x)?[0-9a-f]+$", target) else reg(target)
                continue
            elif op == "ret":
                return s32(regs["rax"])
            elif op in ("mov", "movzx", "movsxd"):
                v = val(a[1], t)
                if op == "movsxd":
                    v = s32(v)
                setreg(a[0], v)
            elif op == "lea":
                setreg(a[0], addr_of(a[1].strip("[]").split("]")[0].lstrip("["), t) if "rip" not in a[1]
                       else addr_of("rip", t))
            elif op in ("add", "sub"):
                v = val(a[0], t) + (val(a[1], t) if op == "add" else -val(a[1], t))
                setreg(a[0], v if a[0] in regs else s32(v))
                flags = (s32(v), 0)
            elif op in ("xor", "or", "and"):
                x, y = val(a[0], t), val(a[1], t)
                setreg(a[0], 0 if (op == "xor" and a[0] == a[1]) else s32({"xor": x ^ y, "or": x | y, "and": x & y}[op]))
                flags = (s32(reg(a[0])), 0)
            elif op in ("sete", "setne"):
                x, y = flags
                b = (x == y) if op == "sete" else (x != y)
                regs["rax"] = (regs["rax"] & ~0xFF) | int(b)
            elif op == "cdqe":
                regs["rax"] = s32(regs["rax"])
            elif op in ("nop", "int3") or op.startswith("nop") or op == "data16":
                pass
            else:
                raise ValueError("unsupported instruction '%s' at 0x%X" % (t, pc))
            pc = nxt
        raise ValueError("no ret within %d steps" % steps)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--rva", required=True, help="the function's RVA")
    ap.add_argument("keys", nargs="+")
    a = ap.parse_args()
    emu = Emu(a.exe)
    for k in a.keys:
        print(k, emu.run(int(a.rva, 0), int(k, 0)))


if __name__ == "__main__":
    main()
