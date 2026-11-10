#!/usr/bin/env python3
"""Writes native routines from lifted SPU code: registers in locals, the float ops native, constants folded.

The lifted code keeps every register in memory and takes the exact float forms op by op. A routine written
here runs under the SPU code's MXCSR with the native ops and one test a pass: MXCSR's overflow and invalid
flags and an operand of exponent 255, as spu_edge_simd.h's fast passes do. When the test fails nothing has
been stored but the stack, and the lifted code does the work.

  loop:  the routine runs at the function's entry, does every pass but the last and leaves the registers the
         loop carries as they would be. The lifted loop goes on from there, with one pass left at least, so
         every register ends as it does in the lifted code. Without "more" the last pass is found by doing
         it: it is undone, as a pass that fails the test is. With "inorder" a pass stores where the lifted
         code does, keeping what it overwrites to undo, for a pass that reads what it has stored.
  whole: the routine does the whole function and writes back every register it set; called functions are
         taken in, and a branch to anywhere else gives the call back to the lifted code.

usage: spu_to_native.py <routines.json> <out.c>
"""
import json
import os
import re
import struct
import sys

ROOT = __file__.replace("\\", "/").rsplit("/scripts/", 1)[0]
# GOWA_WORK and GOWA_ELF: the lift's folder and the decrypted GOWA.elf, when they are not this tree's (scripts/gowa.py).
WORK = (os.environ.get("GOWA_WORK") or ROOT).replace("\\", "/")
FLOAT = {"fm", "fa", "fs", "fma", "fnms", "fms"}
FLOAT_READ = FLOAT | {"fcgt", "fceq", "cflts"}


class Module:
    def __init__(self, name, elf):
        self.name = name
        self.src = open("%s/spu/jobmods/%s/spu_recomp.c" % (WORK, name), encoding="utf-8", errors="replace").read().split("\n")
        man = [m for m in json.load(open(WORK + "/spu/jobmods/jobmods_manifest.json")) if m["name"] == name][0]
        self.base, self.ea = man["base"], man["ea"]
        d = open(elf, "rb").read()
        phoff = struct.unpack(">Q", d[0x20:0x28])[0]
        phsz, phnum = struct.unpack(">HH", d[0x36:0x3A])
        self.segs = []
        for i in range(phnum):
            t, _, off, va, _, fs, _, _ = struct.unpack(">IIQQQQQQ", d[phoff + i * phsz:phoff + i * phsz + 56])
            if t == 1:
                self.segs.append((va, fs, off))
        self.elf = d
        self.heads = {}
        for i, line in enumerate(self.src):
            m = re.match(r"void %s_spu_func_([0-9A-F]{8})\(" % name, line)
            if m:
                self.heads[int(m.group(1), 16)] = i

    def ls(self, a):
        """the quadword at local store address a of the module's image, as four host words"""
        ea = self.ea + a - self.base
        for va, fs, off in self.segs:
            if va <= ea < va + fs:
                return list(struct.unpack(">4I", self.elf[off + ea - va:off + ea - va + 16]))
        raise SystemExit("LS 0x%X is not in the image" % a)

    def body(self, addr):
        i = self.heads[addr] + 1
        out = []
        while not self.src[i].startswith("}"):
            out.append(self.src[i].strip())
            i += 1
        return out


G = r"ctx->gpr\[(\d+)\]"


def parse_addr(s):
    s = s.strip()
    m = re.fullmatch(G + r"\._u32\[0\]\s*\+\s*" + G + r"\._u32\[0\]", s)
    if m:
        return ("rr", int(m.group(1)), int(m.group(2)))
    m = re.fullmatch(G + r"\._u32\[0\](?:\s*\+\s*(-?0x[0-9A-Fa-f]+|-?\d+))?", s)
    if m:
        return ("ri", int(m.group(1)), int(m.group(2) or "0", 0))
    m = re.fullmatch(r"(0x[0-9A-Fa-f]+|\d+)", s)
    if m:
        return ("i", int(s, 0))
    raise SystemExit("address: " + s)


def args_of(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
            continue
        depth += ch in "(["
        depth -= ch in ")]"
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Stmt:
    def __init__(self, op, dst=None, args=(), addr=None, val=None):
        self.op, self.dst, self.args, self.addr, self.val = op, dst, list(args), addr, val


def statements(mod, addr, stop=None, inline=True):
    """the statements from a function's start to its return or the branch back; calls and direct jumps followed"""
    out = []
    lines = mod.body(addr)
    for line in lines:
        if not line or line.startswith(("spu_shufc ", "/*", "spu_sc_", "gowa_", "if (gowa_")) or re.match(r"loc_[0-9A-F]+:", line):
            continue
        if line.startswith("if (ctx->int_enable"):
            continue
        m = re.search(r"spu_link\(0x([0-9A-F]+)\);.*_spu_func_([0-9A-F]{8})\(ctx\);", line)
        if m:
            out.append(Stmt("const", 0, [("link", int(m.group(1), 16))]))
            out += statements(mod, int(m.group(2), 16))
            continue
        m = re.match(r"\{ SPU_DIRECT\(ctx, 0x([0-9A-F]+),", line)
        if m:
            out += statements(mod, int(m.group(1), 16))
            return out
        if line.startswith("SPU_RET"):
            return out
        m = re.match(r"if \(" + G + r"\._u32\[0\] != 0\) \{ SPU_DIRECT", line)
        if m:
            out.append(Stmt("bail_nz", args=[int(m.group(1))]))
            continue
        m = re.match(r"if \(" + G + r"\._u32\[0\] == 0\) goto loc_", line)
        if m:
            out.append(Stmt("back", args=[int(m.group(1))]))
            return out
        m = re.match(r"spu_ls_write128\(ctx, (.*), " + G + r"\);$", line)
        if m:
            out.append(Stmt("store", addr=parse_addr(m.group(1)), val=int(m.group(2))))
            continue
        m = re.match(G + r" = spu_ls_read128\(ctx, (.*)\);$", line)
        if m:
            out.append(Stmt("load", int(m.group(1)), addr=parse_addr(m.group(2))))
            continue
        m = re.match(G + r" = spu_([a-z0-9_]+)\((.*)\);$", line)
        if m:
            args = []
            for a in args_of(m.group(3)):
                g = re.fullmatch(G, a)
                if g:
                    args.append(int(g.group(1)))
                elif a.startswith("&spu_sc_"):
                    args.append(("pat", int(a.split("_")[-1])))
                else:
                    args.append(("imm", int(a, 0)))
            op = m.group(2)
            if op == "shufb_pre":
                op = "shufb"
                args = [args[0], args[1], args[2][1]]
            out.append(Stmt(op, int(m.group(1)), args))
            continue
        raise SystemExit("cannot read: " + line)
    raise SystemExit("function 0x%X ran off its end" % addr)


def host_shuffle(pat):
    """two byte patterns for pshufb on host words, from a shufb control: (from a, from b, or-constant)"""
    a, b, c = [-128] * 16, [-128] * 16, [0] * 16
    for j in range(16):
        w, k = j // 4, 3 - j % 4
        p = (pat[w] >> (24 - 8 * k)) & 0xFF
        if p & 0x80:
            c[j] = 0 if p < 0xC0 else 0xFF if p < 0xE0 else 0x80
        elif p & 0x10:
            q = p & 15
            b[j] = (q // 4) * 4 + 3 - q % 4
        else:
            a[j] = (p // 4) * 4 + 3 - p % 4
    return a, b, c


def vec(bytes16):
    return "_mm_setr_epi8(%s)" % ", ".join(str(x if x < 128 else x - 256) for x in bytes16)


def wordsvec(w):
    return "_mm_setr_epi32(%s)" % ", ".join("(int)0x%08Xu" % x for x in w)


class Gen:
    def __init__(self, mod, cfg):
        self.mod, self.cfg, self.c = mod, cfg, []
        self.known = {int(k): v for k, v in cfg.get("constants", {}).items()}

    def fold(self, st, known):
        """the op's value when every operand is known"""
        op, a = st.op, st.args
        if op == "ila":
            return [a[0][1] & 0x3FFFF] * 4
        if op == "il":
            return [a[0][1] & 0xFFFFFFFF] * 4
        if op == "ilhu":
            return [(a[0][1] & 0xFFFF) << 16] * 4
        if op == "iohl" and a[0] in known:
            return [x | (a[1][1] & 0xFFFF) for x in known[a[0]]]
        if op == "fsmbi":
            w = []
            for i in range(4):
                v = 0
                for k in range(4):
                    if a[0][1] >> (15 - (4 * i + k)) & 1:
                        v |= 0xFF << (24 - 8 * k)
                w.append(v)
            return w
        if op == "andbi" and a[0] in known:
            m = a[1][1] & 0xFF
            return [x & (m * 0x01010101) for x in known[a[0]]]
        if op == "ori" and a[0] in known and a[1][1] == 0:
            return list(known[a[0]])
        if op == "ori" and a[0] in known:
            return [x | (a[1][1] & 0xFFFFFFFF) for x in known[a[0]]]
        if op == "shlqbyi" and a[0] in known and a[1][1] == 0:
            return list(known[a[0]])
        if op == "const":
            return [a[0][1], 0, 0, 0]
        return None

    def undo_lines(self, n):
        return ["    _mm_storeu_si128((__m128i*)(ls + (ua%d & (SPU_LS_SIZE - 16))), uo%d);" % (k, k) for k in range(n - 1, -1, -1)]

    def emit_stmts(self, sts, in_loop=False):
        """C for the statements. Returns the lines of deferred stores."""
        known = dict(self.known)
        late = []
        inorder = self.cfg.get("inorder")
        self.nundo = 0
        self.nlate = 0
        self.undo_vals = []
        flo = self.float_loads(sts)
        need_pz = self.need_pz(sts)
        L = self.c
        for i, st in enumerate(sts):
            op, d, a = st.op, st.dst, st.args
            if op in ("bail_nz",):
                if in_loop:
                    L.append("    if (_mm_cvtsi128_si32(R%d)) {" % a[0])
                    L.extend(self.undo_lines(self.nundo))
                    L.append("    goto out; }")
                else:
                    L.append("    if (_mm_cvtsi128_si32(R%d)) { ec_fl_set(csr); return 0; }" % a[0])
                continue
            if op == "back":
                continue
            if op == "load":
                if st.addr[0] == "i":
                    w = self.mod.ls(st.addr[1])
                    known[d] = w
                    L.append("    R%d = %s;" % (d, wordsvec(w)))
                    continue
                if late and st.addr[1] != 1:
                    raise SystemExit("a load after a late store: this one is for \"inorder\"")
                known.pop(d, None)
                L.append("    R%d = ec_ldq(ls, %s);" % (d, self.addr(st.addr)))
                if i in flo:
                    L.append("    bad = _mm_max_epu32(bad, EXP(R%d));" % d)
                continue
            if op == "store":
                if st.addr[0] == "ri" and st.addr[1] == 1:
                    L.append("    ec_stq(ls, %s, R%d);" % (self.addr(st.addr), st.val))
                elif inorder:
                    k = self.nundo
                    self.nundo += 1
                    L.append("    const uint32_t ua%d = %s;" % (k, self.addr(st.addr)))
                    L.append("    const __m128i uo%d = _mm_loadu_si128((const __m128i*)(ls + (ua%d & (SPU_LS_SIZE - 16))));" % (k, k))
                    L.append("    ec_stq(ls, ua%d, R%d);" % (k, st.val))
                else:
                    tmp = "st%d" % len(late)
                    self.nlate = len(late) + 1
                    L.append("    const uint32_t a_%s = %s; const __m128i v_%s = R%d;" % (tmp, self.addr(st.addr), tmp, st.val))
                    late.append("    ec_stq(ls, a_%s, v_%s);" % (tmp, tmp))
                continue
            v = self.fold(st, known)
            if v is not None:
                known[d] = v
                L.append("    R%d = %s;" % (d, wordsvec(v)))
                continue
            known.pop(d, None)
            L.append("    R%d = %s;" % (d, self.expr(st, known, i in need_pz)))
            # slerp's 1 / sqrt of a zero: an estimate of exponent 255, which only meets a zero, and an
            # SPU product with zero is zero. As a zero here it stays one on the native ops.
            if st.op == "fi" and d == 74 and st.args[0] == 78:
                L.append("    R74 = _mm_and_si128(R74, SI(_mm_cmpneq_ps(PS(R78), _mm_setzero_ps())));")
        return late

    def addr(self, ad):
        if ad[0] == "rr":
            return "(uint32_t)_mm_cvtsi128_si32(R%d) + (uint32_t)_mm_cvtsi128_si32(R%d)" % (ad[1], ad[2])
        if ad[0] == "ri":
            return "(uint32_t)_mm_cvtsi128_si32(R%d) + (uint32_t)%d" % (ad[1], ad[2])
        return "0x%Xu" % ad[1]

    def expr(self, st, known, pz):
        op, a = st.op, st.args
        r = lambda i: "R%d" % a[i]
        imm = lambda i: a[i][1]
        if op in FLOAT:
            e = {"fm": "FM(%s, %s)", "fa": "FA(%s, %s)", "fs": "FS(%s, %s)", "fma": "FMA(%s, %s, %s)",
                 "fnms": "FNMS(%s, %s, %s)", "fms": "FMS(%s, %s, %s)"}[op] % tuple(r(i) for i in range(len(a)))
            return "PZ(%s)" % e if pz else e
        if op == "shufb":
            pat_reg = a[2]
            if pat_reg not in known:
                raise SystemExit("shufb with an unknown pattern in r%d" % pat_reg)
            pa, pb, pc = host_shuffle(known[pat_reg])
            parts = []
            if any(x >= 0 for x in pa):
                parts.append("_mm_shuffle_epi8(%s, %s)" % (r(0), vec([x & 0xFF for x in pa])))
            if any(x >= 0 for x in pb):
                parts.append("_mm_shuffle_epi8(%s, %s)" % (r(1), vec([x & 0xFF for x in pb])))
            if any(pc):
                parts.append(vec(pc))
            e = parts[0] if parts else "_mm_setzero_si128()"
            for p in parts[1:]:
                e = "_mm_or_si128(%s, %s)" % (e, p)
            return e
        simple = {
            "and": "_mm_and_si128(%s, %s)", "or": "_mm_or_si128(%s, %s)", "xor": "_mm_xor_si128(%s, %s)",
            "a": "_mm_add_epi32(%s, %s)", "cgt": "_mm_cmpgt_epi32(%s, %s)", "ceq": "_mm_cmpeq_epi32(%s, %s)",
        }
        if op in simple:
            return simple[op] % (r(0), r(1))
        if op == "andc":
            return "_mm_andnot_si128(%s, %s)" % (r(1), r(0))
        if op == "sf":
            return "_mm_sub_epi32(%s, %s)" % (r(1), r(0))
        if op == "selb":
            return "SELB(%s, %s, %s)" % (r(0), r(1), r(2))
        if op == "ai":
            return "_mm_add_epi32(%s, _mm_set1_epi32(%d))" % (r(0), imm(1))
        if op == "andi":
            return "_mm_and_si128(%s, _mm_set1_epi32(%d))" % (r(0), imm(1))
        if op == "ori":
            return "_mm_or_si128(%s, _mm_set1_epi32(%d))" % (r(0), imm(1))
        if op == "andbi":
            return "_mm_and_si128(%s, _mm_set1_epi8((char)%d))" % (r(0), imm(1))
        if op == "cgti":
            return "_mm_cmpgt_epi32(%s, _mm_set1_epi32(%d))" % (r(0), imm(1))
        if op == "ceqi":
            return "_mm_cmpeq_epi32(%s, _mm_set1_epi32(%d))" % (r(0), imm(1))
        if op == "shli":
            return "_mm_slli_epi32(%s, %d)" % (r(0), imm(1))
        if op == "rotmi":
            return "_mm_srli_epi32(%s, %d)" % (r(0), -imm(1))
        if op == "rotmai":
            return "_mm_srai_epi32(%s, %d)" % (r(0), min(-imm(1), 31))
        if op == "shlqbyi":
            return "_mm_srli_si128(%s, %d)" % (r(0), imm(1)) if imm(1) % 4 == 0 else self.slow(st)
        if op == "rotqbyi":
            n = imm(1) & 15
            if n % 4 == 0:
                w = n // 4
                return "_mm_shuffle_epi32(%s, 0x%02X)" % (r(0), w | ((w + 1) & 3) << 2 | ((w + 2) & 3) << 4 | ((w + 3) & 3) << 6)
            return self.slow(st)
        if op == "fcgt":
            return "_mm_castps_si128(_mm_cmpgt_ps(_mm_castsi128_ps(%s), _mm_castsi128_ps(%s)))" % (r(0), r(1))
        if op == "fceq":
            return "_mm_castps_si128(_mm_cmpeq_ps(_mm_castsi128_ps(%s), _mm_castsi128_ps(%s)))" % (r(0), r(1))
        if op == "frest":
            return "FREST(%s)" % r(0)
        if op == "frsqest":
            return "FRSQEST(%s)" % r(0)
        if op == "fi":
            return "FI(%s, %s)" % (r(0), r(1))
        if op == "cflts" and imm(1) == 173:
            return "_mm_cvttps_epi32(_mm_castsi128_ps(%s))" % r(0)
        if op == "csflt" and imm(1) == 155:
            return "_mm_castps_si128(_mm_cvtepi32_ps(%s))" % r(0)
        return self.slow(st)

    def slow(self, st):
        a = st.args
        parts = []
        for x in a:
            parts.append("ec_u(R%d)" % x if isinstance(x, int) else str(x[1]))
        return "ec_m(spu_%s(%s))" % (st.op, ", ".join(parts))

    def float_loads(self, sts):
        """the loads whose value reaches a float op"""
        moves = {"shufb", "selb", "and", "andc", "or", "xor", "rotqbyi", "shlqbyi", "ori"}
        want = set()
        out = set()
        for i in range(len(sts) - 1, -1, -1):
            st = sts[i]
            if st.dst is not None and st.dst in want:
                want.discard(st.dst)
                if st.op == "load":
                    out.add(i)
                elif st.op in moves:
                    # selb and shufb move both; a mask's value is no float
                    n = 2 if st.op in ("selb", "shufb") else 1
                    want |= {x for x in st.args[:n] if isinstance(x, int)}
            if st.op in FLOAT_READ:
                want |= {x for x in st.args if isinstance(x, int)}
        self.float_live_in = want
        return out

    def need_pz(self, sts):
        """Float results whose bits something reads: they take the +0 rule. A zero's sign never changes
        a float op's non-zero result, so a value that only moves on to float ops does not need it."""
        moves = {"shufb", "selb", "and", "andc", "rotqbyi", "shlqbyi", "ori"}
        out = set()
        want = {r for r in range(128)}       # what is left in a register at the end
        for i in range(len(sts) - 1, -1, -1):
            st = sts[i]
            d = st.dst
            needed = d is not None and d in want
            if d is not None:
                want.discard(d)
                if st.op in FLOAT and needed:
                    out.add(i)
            if st.op == "store":
                want.add(st.val)
            elif st.op in FLOAT or st.op in ("fcgt", "fceq", "cflts"):
                pass
            elif st.op in moves:
                if needed:
                    n = 2 if st.op in ("selb", "shufb") else 1
                    want |= {x for x in st.args[:n] if isinstance(x, int)}
            else:
                want |= {x for x in st.args if isinstance(x, int)}
        return out

    def regs_read_first(self, sts):
        seen, first = set(), []
        for st in sts:
            uses = [x for x in st.args if isinstance(x, int)]
            if st.op == "store":
                uses = uses + [st.val]
            if st.addr and st.addr[0] != "i":
                uses += [st.addr[1]] + ([st.addr[2]] if st.addr[0] == "rr" else [])
            for u in uses:
                if u not in seen and u not in first:
                    first.append(u)
            if st.dst is not None:
                seen.add(st.dst)
        return first

    def whole(self):
        cfg = self.cfg
        sts = statements(self.mod, int(cfg["func"], 16))
        live = self.regs_read_first(sts)
        written = sorted({st.dst for st in sts if st.dst is not None})
        allr = sorted(set(live) | set(written))
        L = self.c
        L.append("int %s(spu_context* ctx)\n{" % cfg["name"])
        L.append("    if (!g6r_on() || (_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return 0;")
        L.append("    uint8_t* const ls = ctx->ls;")
        L.append("    const unsigned csr = _mm_getcsr();")
        L.append("    __m128i bad = _mm_setzero_si128();")
        L.append("    __m128i %s;" % ", ".join("R%d" % r for r in allr))
        for r in live:
            L.append("    R%d = ec_m(ctx->gpr[%d]);" % (r, r))
        self.float_loads(sts)
        for r in sorted(self.float_live_in & set(live)):
            L.append("    bad = _mm_max_epu32(bad, EXP(R%d));" % r)
        L.append("    ec_fl_clear(csr);")
        for r in sorted(self.float_live_in & set(live)):
            L.append("    EC_FL_KEEP(R%d);" % r)
        late = self.emit_stmts(sts)
        outs = [r for r in written if r <= 3 or r >= 80]    # the ABI's: r4-r79 are dead at a return
        for r in outs:
            L.append("    EC_FL_USEI128(R%d);" % r)
        for k in range(self.nlate):
            L.append("    EC_FL_USEI128(v_st%d);" % k)
        L.append("    const unsigned fl = ec_fl_read();")
        L.append("    ec_fl_set(csr);")
        L.append("    if ((fl & EC_FLAGS_BAD) || BAD255(bad)) {")
        L.extend(self.undo_lines(self.nundo))
        L.append("    G6R_COUNT(%s, redo); return 0; }" % cfg["short"])
        L += late
        for r in outs:
            L.append("    ctx->gpr[%d] = ec_u(R%d);" % (r, r))
        L.append("    G6R_COUNT(%s, done);" % cfg["short"])
        L.append("    return 1;\n}\n")

    def loop(self):
        cfg = self.cfg
        sts = statements(self.mod, int(cfg["func"], 16))
        if sts[-1].op != "back":
            raise SystemExit("no branch back in " + cfg["name"])
        live = self.regs_read_first(sts)
        written = sorted({st.dst for st in sts if st.dst is not None})
        carried = [r for r in live if r in written]
        allr = sorted(set(live) | set(written))
        tmpl = {r: "N%d" % r for r in carried}
        L = self.c
        L.append("void %s(spu_context* ctx)\n{" % cfg["name"])
        L.append("    if (!g6r_on() || (_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return;")
        for k, v in sorted(self.known.items()):
            L.append("    if (!_mm_test_all_ones(_mm_cmpeq_epi32(ec_m(ctx->gpr[%d]), %s))) { G6R_COUNT(%s, declined); return; }"
                     % (k, wordsvec(v), cfg["short"]))
        L.append("    uint8_t* const ls = ctx->ls;")
        L.append("    const unsigned csr = _mm_getcsr();")
        L.append("    __m128i %s;" % ", ".join("R%d" % r for r in allr))
        for r in live:
            L.append("    R%d = ec_m(ctx->gpr[%d]);" % (r, r))
        L.append("    __m128i %s;" % ", ".join("C%d = R%d" % (r, r) for r in carried))
        self.float_loads(sts)
        L.append("    __m128i bad0 = _mm_setzero_si128();")
        for r in sorted(self.float_live_in & set(live)):
            L.append("    bad0 = _mm_or_si128(bad0, E255(R%d));" % r)
        L.append("    if (!_mm_testz_si128(bad0, bad0)) { G6R_COUNT(%s, declined); return; }" % cfg["short"])
        L.append("    uint32_t passes = 0;")
        L.append("    ec_fl_clear(csr);")
        for r in sorted(self.float_live_in & set(live)):
            L.append("    EC_FL_KEEP(R%d);" % r)
        L.append("    while (%s) {" % cfg.get("more", "1"))
        L.append("    __m128i bad = _mm_setzero_si128();")
        for r in carried:
            L.append("    R%d = C%d;" % (r, r))
        late = self.emit_stmts(sts, True)
        for r in carried:
            L.append("    EC_FL_USEI128(R%d);" % r)
        for k in range(self.nlate):
            L.append("    EC_FL_USEI128(v_st%d);" % k)
        for k in self.undo_vals:
            L.append("    EC_FL_USEI128(%s);" % k)
        L.append("    const unsigned fl = ec_fl_read();")
        L.append("    if ((fl & EC_FLAGS_BAD) || BAD255(bad)) {")
        L.extend(self.undo_lines(self.nundo))
        L.append("    G6R_COUNT(%s, redo); goto out; }" % cfg["short"])
        if "more" not in cfg:
            L.append("    if (_mm_cvtsi128_si32(R%d)) {" % sts[-1].args[0])
            L.extend(self.undo_lines(self.nundo))
            L.append("    goto out; }")
        L += late
        for r in carried:
            L.append("    C%d = R%d;" % (r, r))
        L.append("    passes++;")
        L.append("    }")
        L.append("out:")
        L.append("    ec_fl_set(csr);")
        for r in carried:
            L.append("    ctx->gpr[%d] = ec_u(C%d);" % (r, r))
        L.append("    G6R_COUNT(%s, done);" % cfg["short"])
        L.append("    (void)passes;\n}\n")


HEAD = """/*
 * General6's routines as native code: written by scripts/spu_to_native.py from the lifted module, not by
 * hand. GOWA_G6_NATIVE=0 leaves them to the lifted code; bit 2 of it off, these only.
 *
 * Registers in locals, the float ops native under the SPU code's MXCSR, one test a pass, as the fast
 * passes of spu_edge_simd.h: an operand of exponent 255 or MXCSR's overflow or invalid flag gives the
 * work to the lifted code, with nothing stored but the stack. The +0 rule is applied to a float result
 * that anything but a float op reads.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

#define PS(x) _mm_castsi128_ps(x)
#define SI(x) _mm_castps_si128(x)
#define FM(a, b) SI(_mm_mul_ps(PS(a), PS(b)))
#define FA(a, b) SI(_mm_add_ps(PS(a), PS(b)))
#define FS(a, b) SI(_mm_sub_ps(PS(a), PS(b)))
#define FMA(a, b, c) SI(_mm_fmadd_ps(PS(a), PS(b), PS(c)))
#define FNMS(a, b, c) SI(_mm_fnmadd_ps(PS(a), PS(b), PS(c)))
#define FMS(a, b, c) SI(_mm_fmsub_ps(PS(a), PS(b), PS(c)))
#define EC_FL_USEI128(v) __asm__ volatile("" : : "x"(v))
#define EXP(x) _mm_and_si128((x), _mm_set1_epi32(0x7F800000))
#define BAD255(b) (!_mm_testz_si128(_mm_cmpeq_epi32((b), _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(-1)))
EC_INL __m128i PZ(__m128i r) { return _mm_and_si128(r, SI(_mm_cmpneq_ps(PS(r), _mm_setzero_ps()))); }
EC_INL __m128i SELB(__m128i a, __m128i b, __m128i m) { return _mm_or_si128(_mm_and_si128(m, b), _mm_andnot_si128(m, a)); }
EC_INL __m128i E255(__m128i x)
{
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_cmpeq_epi32(_mm_and_si128(x, e), e);
}
EC_INL __m128i FREST(__m128i x) { return _mm256_castsi256_si128(ec_frest(_mm256_castsi128_si256(x))); }
EC_INL __m128i FRSQEST(__m128i x) { return _mm256_castsi256_si128(ec_frsqest(_mm256_castsi128_si256(x))); }
EC_INL __m128i FI(__m128i a, __m128i e)
{
    return _mm256_castsi256_si128(ec_fi(_mm256_castsi128_si256(a), _mm256_castsi128_si256(e)));
}

static int g6r_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GOWA_G6_NATIVE"); on = !e || (atoi(e) & 2) != 0; }
    return on;
}
static int g6r_stats_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
    return on;
}
"""


def main():
    spec = json.load(open(sys.argv[1]))
    mod = Module(spec["module"], os.environ.get("GOWA_ELF") or spec["elf"])
    out = [HEAD]
    shorts = [r["short"] for r in spec["routines"]]
    for s in shorts:
        out.append("static volatile long long s_%s_done, s_%s_redo, s_%s_declined;" % (s, s, s))
    out.append("static void g6r_report(void)\n{")
    for s in shorts:
        out.append('    fprintf(stderr, "[g6-native] %s: %%lld done, %%lld left to the lifted code in a pass, %%lld declined\\n", s_%s_done, s_%s_redo, s_%s_declined);' % (s, s, s, s))
    out.append("}")
    out.append("static void g6r_tick(volatile long long* n)\n{")
    out.append("    static volatile long once;")
    out.append("    if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED)) atexit(g6r_report);")
    out.append("    if (!(__atomic_add_fetch(n, 1, __ATOMIC_RELAXED) & 0xFFFFF)) g6r_report();")
    out.append("}")
    out.append("#define G6R_COUNT(n, k) do { if (g6r_stats_on()) g6r_tick(&s_##n##_##k); } while (0)\n")
    for r in spec["routines"]:
        g = Gen(mod, r)
        (g.loop if r["kind"] == "loop" else g.whole)()
        out += g.c
    out.append("#else")
    for r in spec["routines"]:
        out.append(("void %s(spu_context* ctx) { (void)ctx; }" if r["kind"] == "loop" else "int %s(spu_context* ctx) { (void)ctx; return 0; }") % r["name"])
    out.append("#endif\n")
    open(sys.argv[2], "w", newline="\n").write("\n".join(out))
    print("wrote", sys.argv[2], sum(len(x.split("\n")) for x in out), "lines")


main()
