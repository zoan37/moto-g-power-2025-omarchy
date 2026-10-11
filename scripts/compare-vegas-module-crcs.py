#!/usr/bin/env python3
"""Compare the CRCs that vendor .ko files import against a kernel's (vmlinux.)symvers.

usage: scripts/compare-vegas-module-crcs.py SYMVERS MODDIR [MODDIR...]
Prints match/mismatch/missing counts for symbols provided by vmlinux; symbols that
are exported by another vendor module (EXPORT_SYMBOL in a .ko) are counted separately.
"""
import subprocess, sys, pathlib, collections, hashlib

def symvers(path):
    d = {}
    for line in open(path):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 3:
            d[p[1]] = (int(p[0], 16), p[2])
    return d

def imports(ko):
    out = subprocess.run(["modprobe", "--dump-modversions", str(ko)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        crc, name = line.split("\t")
        yield name, int(crc, 16)

def ksymtab(ko):
    # names of symbols exported by a module: __ksymtab_<name> symbols
    out = subprocess.run(["llvm-nm", "--defined-only", str(ko)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith("__ksymtab_"):
            yield p[2][len("__ksymtab_"):]

kern = symvers(sys.argv[1])
kos = sorted({p.resolve() for d in sys.argv[2:] for p in pathlib.Path(d).rglob("*.ko")}, key=lambda p: p.name)
# dedupe identical files (vendor_boot and vendor_dlkm share some modules)
seen = {}
for k in kos:
    seen.setdefault(hashlib.sha256(k.read_bytes()).hexdigest(), k)
mod_exports = set()
for k in seen.values():
    mod_exports.update(ksymtab(k))
need = {}
conflict = 0
for k in seen.values():
    for name, crc in imports(k):
        if name in need and need[name] != crc:
            conflict += 1
        need.setdefault(name, crc)
stat = collections.Counter()
mism, miss = [], []
for name, crc in sorted(need.items()):
    if name in kern:
        if kern[name][0] == crc:
            stat["match"] += 1
        else:
            stat["mismatch"] += 1; mism.append(name)
    elif name in mod_exports:
        stat["from_vendor_module"] += 1
    else:
        stat["missing"] += 1; miss.append(name)
print(f"ko_files={len(kos)} distinct_modules={len(seen)} unique_imported_symbols={len(need)} intra_vendor_crc_conflicts={conflict}")
print(" ".join(f"{k}={v}" for k, v in sorted(stat.items())))
if mism: print("MISMATCH:", " ".join(mism[:400]))
if miss: print("MISSING:", " ".join(miss[:400]))
