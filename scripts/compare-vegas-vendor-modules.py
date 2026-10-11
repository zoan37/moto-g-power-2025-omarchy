#!/usr/bin/env python3
"""Compare rebuilt vendor .ko files with the stock ones (same file name).

usage: scripts/compare-vegas-vendor-modules.py [--all] STOCK_DIR[,STOCK_DIR...] BUILT [BUILT...]
  BUILT is a .ko file or a directory searched recursively for .ko files.
  For every built module that has a stock counterpart, print:
    vermagic / depends equal, __versions (every stock import: same name and CRC
    in the rebuild, extra or missing imports), exported symbols + CRCs
    (__crc_* symbols), the sizes of all SHF_ALLOC sections, and which non-debug
    sections differ byte for byte (.debug* and .rela.debug* are skipped).
  --all: one summary line per module instead of full detail; exit status 1 if
  any module differs in vermagic/imports/exports.
Needs modinfo, modprobe (--dump-modversions) and llvm-nm/llvm-readelf in PATH.
"""
import pathlib, struct, subprocess, sys

def run(*a):
    return subprocess.run(a, capture_output=True, text=True, check=True).stdout

def modinfo(ko, field):
    return run("modinfo", "-F", field, str(ko)).strip()

def versions(ko):
    d = {}
    for line in run("modprobe", "--dump-modversions", str(ko)).splitlines():
        crc, name = line.split("\t")
        d[name] = int(crc, 16)
    return d

def exports(ko):
    d = {}
    for line in run("llvm-nm", "--defined-only", str(ko)).splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith("__crc_"):
            d[p[2][6:]] = int(p[0], 16)
    return d

def alloc_sections(ko):
    # llvm-readelf -S --wide; keep SHF_ALLOC sections (flag A), name -> size
    d = {}
    for line in run("llvm-readelf", "-S", "-W", str(ko)).splitlines():
        line = line.strip()
        if not line.startswith("["):
            continue
        rest = line.split("]", 1)[1].split()
        if len(rest) < 7:
            continue
        name, typ, size, flg = rest[0], rest[1], rest[4], rest[6]
        if "A" in flg and not flg.isdigit():
            d[name] = int(size, 16)
    return d

def section_bytes(ko):
    # ELF64 little-endian section contents by name (NOBITS -> b"")
    b = ko.read_bytes()
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x3a)
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)]
    stro = hdrs[shstrndx][4]
    d = {}
    for h in hdrs:
        name = b[stro + h[0]:b.index(b"\0", stro + h[0])].decode()
        d[name] = b"" if h[1] == 8 else b[h[4]:h[4] + h[5]]
    return d

def compare(stock, built, detail):
    out, bad = [], False
    vs, vb = modinfo(stock, "vermagic"), modinfo(built, "vermagic")
    ds, db = modinfo(stock, "depends"), modinfo(built, "depends")
    out.append(f"vermagic: {'same' if vs == vb else 'DIFF'} ({vb})")
    out.append(f"depends: {'same' if set(ds.split(',')) == set(db.split(',')) else 'DIFF'} ({db})")
    bad |= vs != vb
    is_, ib = versions(stock), versions(built)
    mism = sorted(n for n in is_ if n in ib and is_[n] != ib[n])
    miss = sorted(set(is_) - set(ib))
    extra = sorted(set(ib) - set(is_))
    out.append(f"imports: stock {len(is_)}, rebuilt {len(ib)}, CRC match {len(is_) - len(mism) - len(miss)}, "
               f"CRC mismatch {len(mism)}, not imported by rebuild {len(miss)}, extra in rebuild {len(extra)}")
    for n in mism:
        out.append(f"  CRC MISMATCH {n}: stock {is_[n]:#010x} rebuilt {ib[n]:#010x}")
    for n in miss:
        out.append(f"  only stock imports {n}")
    for n in extra:
        out.append(f"  only rebuild imports {n} {ib[n]:#010x}")
    bad |= bool(mism or miss or extra)
    es, eb = exports(stock), exports(built)
    emism = sorted(n for n in es if n in eb and es[n] != eb[n])
    emiss, eextra = sorted(set(es) - set(eb)), sorted(set(eb) - set(es))
    out.append(f"exports: stock {len(es)}, rebuilt {len(eb)}, CRC mismatch {len(emism)}, "
               f"missing {len(emiss)}, extra {len(eextra)}")
    for n in emism:
        out.append(f"  EXPORT CRC MISMATCH {n}: stock {es[n]:#010x} rebuilt {eb[n]:#010x}")
    for n in emiss + eextra:
        out.append(f"  export only in {'stock' if n in es else 'rebuild'}: {n}")
    bad |= bool(emism or emiss or eextra)
    ss, sb = alloc_sections(stock), alloc_sections(built)
    sdiff = [n for n in sorted(set(ss) | set(sb)) if ss.get(n) != sb.get(n)]
    out.append(f"alloc sections: {len(ss)} stock, {len(sb)} rebuilt, size differences {len(sdiff)}")
    for n in sdiff:
        out.append(f"  {n}: stock {ss.get(n)} rebuilt {sb.get(n)}")
    bs, bb = section_bytes(stock), section_bytes(built)
    names = [n for n in sorted(set(bs) | set(bb)) if not n.startswith((".debug", ".rela.debug"))]
    bdiff = [n for n in names if bs.get(n) != bb.get(n)]
    out.append(f"section contents: {len(names) - len(bdiff)} identical, differ: {' '.join(bdiff) or '-'}")
    return out, bad, len(sdiff)

def main():
    args = sys.argv[1:]
    summary = args and args[0] == "--all"
    if summary:
        args = args[1:]
    stock = {}
    for d in args[0].split(","):
        for p in pathlib.Path(d).rglob("*.ko"):
            stock.setdefault(p.name, p)
    built = []
    for a in args[1:]:
        p = pathlib.Path(a)
        built += sorted(p.rglob("*.ko")) if p.is_dir() else [p]
    nbad = nsec = n = 0
    for b in built:
        s = stock.get(b.name)
        if s is None:
            continue
        n += 1
        lines, bad, nsd = compare(s, b, not summary)
        nbad += bad
        nsec += nsd > 0
        if summary:
            print(f"{'DIFF' if bad else 'ok  '} sections_differ={nsd:<3} {b.name}  "
                  + " | ".join(l for l in lines if l.startswith(("imports", "exports"))))
        else:
            print(f"== {b.name}  (stock {s})")
            print("\n".join(lines))
    if summary:
        print(f"modules compared {n}, with vermagic/import/export differences {nbad}, "
              f"with alloc-section size differences {nsec}")
    sys.exit(1 if nbad else 0)

main()
