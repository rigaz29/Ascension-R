"""The D3D12 backend's shader cache store, checked (libs/video/rsx_d3d12_backend.c, "Compiled guest shaders on
disk"): the pack (PS3RSHC3, one zstd frame) and <pack>.journal (PS3RSHJ1) read the way the game reads them, every
record whole, every program under the hash of its bytes, every source, pipeline and use naming what the store has.
Given the files from before the store (PS3RSHC2 programs, GWPSOC01 pipelines), also that each of their sources and
pipelines is in the store with the same bytes.

usage: py tools/shader_cache_check.py <pack> [<old programs> [<old pipelines>]]
       (Python 3.14 or later, for compression.zstd)"""
import collections, os, struct, sys
from compression import zstd

M = (1 << 64) - 1
PROG, SRCK, PIPE, USED = 0x474F5250, 0x4B435253, 0x45504950, 0x44455355


def fnv(b):
    """A program's id: FNV-1a 64 of its bytes, then of their length; never 0."""
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & M
    h = ((h ^ len(b)) * 1099511628211) & M
    return h or 1


class Store:
    def __init__(self):
        self.progs, self.srcs, self.pipes, self.uses, self.bad = {}, {}, {}, collections.Counter(), []

    def read(self, b, start):
        """The records of b from start on; returns how far they were whole."""
        o = start
        while len(b) - o >= 8:
            tag, rest = struct.unpack_from("<II", b, o)
            if rest & 3 or rest > len(b) - o - 8:
                break
            r = o + 8
            if tag == PROG:
                pid, ln = struct.unpack_from("<QI", b, r)
                if (ln + 3) & ~3 != rest - 16:
                    break
                body = bytes(b[r + 16:r + 16 + ln])
                if fnv(body) != pid:
                    self.bad.append(("PROG under another id", pid))
                elif self.progs.setdefault(pid, body) != body:
                    self.bad.append(("PROG twice with other bytes", pid))
            elif tag == SRCK:
                key, sl, _, pid = struct.unpack_from("<QIIQ", b, r)
                if pid not in self.progs:
                    self.bad.append(("SRCK names no program", key))
                else:
                    self.srcs.setdefault((key, sl), pid)
            elif tag == PIPE:
                key, vs, ps, used, stn = struct.unpack_from("<QQQII", b, r)
                if (stn + 3) & ~3 != rest - 32:
                    break
                if vs not in self.progs or ps not in self.progs:
                    self.bad.append(("PIPE names no program", key))
                else:
                    self.pipes.setdefault(key, [vs, ps, used, bytes(b[r + 32:r + 32 + stn])])
            elif tag == USED:
                when, k = struct.unpack_from("<II", b, r)
                if 8 * k != rest - 8:
                    break
                for j in range(k):
                    (key,) = struct.unpack_from("<Q", b, r + 8 + 8 * j)
                    if key not in self.pipes:
                        self.bad.append(("USED names no pipeline", key))
                    else:
                        self.pipes[key][2] = max(self.pipes[key][2], when)
                    self.uses[when] += 1
            o += 8 + rest
        return o

    def counts(self):
        return len(self.progs), len(self.srcs), len(self.pipes)


def main():
    pack = sys.argv[1]
    s = Store()
    d = open(pack, "rb").read()
    if d[:8] != b"PS3RSHC3":
        sys.exit("%s: not a pack (%r)" % (pack, d[:8]))
    hs, _, raw, cz, jid, jlen = struct.unpack_from("<IIQQQQ", d, 8)
    if hs != 48 or cz != len(d) - 48:
        sys.exit("%s: a damaged header" % pack)
    pay = zstd.decompress(d[48:])
    end = s.read(pay, 0)
    print("%s: %.2f MB packed, %.2f MB unpacked, read to %d of %d; %d programs, %d sources, %d pipelines" % (
        os.path.basename(pack), len(d) / 1048576, raw / 1048576, end, len(pay), *s.counts()))
    jp = pack + ".journal"
    if os.path.exists(jp):
        j = open(jp, "rb").read()
        if j[:8] != b"PS3RSHJ1":
            sys.exit("%s: not a journal (%r)" % (jp, j[:8]))
        (my,) = struct.unpack_from("<Q", j, 8)
        start = jlen if my == jid and 16 <= jlen <= len(j) else 16
        n0 = s.counts()
        e = s.read(j, start)
        print("journal: %d bytes, read from %d%s to %d; +%d programs, +%d sources, +%d pipelines" % (
            len(j), start, " (the pack has what is before)" if start > 16 else "", e,
            *(a - b for a, b in zip(s.counts(), n0))))
    print("uses by session: %s" % dict(sorted(s.uses.items())))
    print("pipelines by the session that last used them: %s" % sorted(
        collections.Counter(p[2] for p in s.pipes.values()).items()))
    print("problems: %d %s" % (len(s.bad), s.bad[:5]))

    if len(sys.argv) > 2:
        o = open(sys.argv[2], "rb").read()
        if o[:8] != b"PS3RSHC2":
            sys.exit("%s: not PS3RSHC2" % sys.argv[2])
        i, n, miss = 8, 0, 0
        while i + 16 <= len(o):
            key, sl, ln = struct.unpack_from("<QII", o, i)
            body = o[i + 16:i + 16 + ln]
            i += 16 + ln
            n += 1
            pid = s.srcs.get((key, sl))
            miss += pid is None or s.progs[pid] != body
        print("old programs: %d sources, %d missing or different" % (n, miss))
    if len(sys.argv) > 3:
        o = open(sys.argv[3], "rb").read()
        if o[:8] != b"GWPSOC01":
            sys.exit("%s: not GWPSOC01" % sys.argv[3])
        i, n, miss = 8, 0, 0
        while i + 12 <= len(o):
            key, stn = struct.unpack_from("<QI", o, i); i += 12
            st = o[i:i + stn]; i += stn
            (vn,) = struct.unpack_from("<I", o, i); i += 4
            vs = o[i:i + vn]; i += vn
            (pn,) = struct.unpack_from("<I", o, i); i += 4
            ps = o[i:i + pn]; i += pn
            n += 1
            p = s.pipes.get(key)
            miss += p is None or p[3] != st or s.progs[p[0]] != vs or s.progs[p[1]] != ps
        print("old pipelines: %d, %d missing or different" % (n, miss))


if __name__ == "__main__":
    main()
