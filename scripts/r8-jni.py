#!/usr/bin/env python3
"""r8-jni.py — the Kotlin classes and methods our C++ reaches BY NAME, read from the C++ itself.

★★★ WHY. R8 renames and removes whatever no Java/Kotlin code appears to use. The native server
    (libvibelocalsdr, android/app/src/main/cpp) reaches into Kotlin by name in two ways, and R8 sees neither:
      1. exported `Java_com_vibesdr_app_<Class>_<method>` functions — the `external fun`s that call INTO C++;
      2. FindClass("com/vibesdr/app/…") + GetStaticMethodID / GetMethodID(cls, "name", "sig") — C++ calling BACK
         into Kotlin (VibeHttp.get, VibeMapGL.download, VibeMapGL.fetchIf). Nothing in Kotlin calls these, so R8
         would delete them, and the first High Detail map download or ADS-B data update would fail at run time.
    The list is generated from the C++ on every release build, so a new JNI call in C++ is kept without anybody
    remembering to write a rule — and `check` reads the R8 OUTPUT (the dex) and fails the build if any of it is
    missing, renamed, or has a different signature from the one the C++ asks for.

usage:
  r8-jni.py list  --cpp DIR                                   print what the C++ references
  r8-jni.py keep  --cpp DIR --out FILE.pro                    write the R8 keep rules
  r8-jni.py check --cpp DIR --src DIR [--src DIR…] DEX…       exit 1 = something the C++ needs is not in the dex
        DEX = a .dex file, a directory of them, or an .apk / .aab (every classes*.dex inside)
        --src = the Kotlin/Java source roots compiled into THIS app. A class the C++ names but no source declares
                (Lite compiles only part of the main app's Kotlin) is reported and skipped, not failed.

★ Context rule for GetMethodID(cls, …): `cls` is the class of the nearest preceding FindClass("…") or enclosing
  Java_… function, whichever is closer. If C++ ever looks a method up some other way, `check` will report it as
  missing from the wrong class — a loud false alarm, never a silent pass.
"""
import argparse
import os
import re
import struct
import sys
import zipfile

SKIP_DIRS = {"sdr-kit", "cloudflared", "build", ".cxx", "include"}
CPP_EXT = (".cpp", ".cc", ".c", ".h", ".hpp")


# ── C++ side ────────────────────────────────────────────────────────────────────────────────────────────
def strip_comments(src):
    """Remove // and /* */ comments but leave string literals alone (a URL's // is not a comment)."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1]); i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2); seg = src[i:(n if j < 0 else j + 2)]
            out.append("\n" * seg.count("\n")); i = n if j < 0 else j + 2
        else:
            out.append(c); i += 1
    return "".join(out)


def jni_unmangle(sym):
    """Java_com_vibesdr_app_VibeMapGL_nativeInit → ('com.vibesdr.app.VibeMapGL', 'nativeInit')."""
    s, out, i = sym[len("Java_"):], [], 0
    while i < len(s):
        c = s[i]
        if c == "_":
            nxt = s[i + 1] if i + 1 < len(s) else ""
            if nxt == "1": out.append("_"); i += 2; continue
            if nxt == "2": out.append(";"); i += 2; continue
            if nxt == "3": out.append("["); i += 2; continue
            if nxt == "0": out.append(chr(int(s[i + 2:i + 6], 16))); i += 6; continue
            if nxt == "_": break                      # overloaded: __<signature> follows
            out.append("/"); i += 1; continue
        out.append(c); i += 1
    path = "".join(out)
    cls, _, meth = path.rpartition("/")
    return cls.replace("/", "."), meth


def scan_cpp(cpp_dir):
    """→ {class: {'natives': set(names), 'calls': set((name, sig, static))}}"""
    refs = {}
    tok = re.compile(r'(?P<java>\bJava_[A-Za-z0-9_]+)\s*\('
                     r'|FindClass\s*\(\s*"(?P<find>[^"]+)"'
                     r'|Get(?P<static>Static)?(?P<kind>Method|Field)ID\s*\(\s*[^,]+,\s*"(?P<name>[^"]+)"\s*,\s*"(?P<sig>[^"]+)"',
                     re.S)
    for root, dirs, files in os.walk(cpp_dir):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for f in sorted(files):
            if not f.endswith(CPP_EXT):
                continue
            path = os.path.join(root, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                src = strip_comments(fh.read())
            ctx = None
            for m in tok.finditer(src):
                if m.group("java"):
                    cls, meth = jni_unmangle(m.group("java"))
                    refs.setdefault(cls, {"natives": set(), "calls": set(), "fields": set()})["natives"].add(meth)
                    ctx = cls
                elif m.group("find"):
                    ctx = m.group("find").replace("/", ".")
                    refs.setdefault(ctx, {"natives": set(), "calls": set(), "fields": set()})
                else:
                    if ctx is None:
                        sys.exit("r8-jni: %s: Get%sID(\"%s\") with no class context" % (path, m.group("kind"), m.group("name")))
                    if not ctx.startswith("com.vibesdr."):
                        continue                    # java.*/android.* — the platform's, never shrunk
                    key = "calls" if m.group("kind") == "Method" else "fields"
                    refs[ctx][key].add((m.group("name"), m.group("sig"), bool(m.group("static"))))
    return {c: r for c, r in refs.items() if c.startswith("com.vibesdr.")}


# ── keep rules ──────────────────────────────────────────────────────────────────────────────────────────
PRIM = {"Z": "boolean", "B": "byte", "C": "char", "S": "short", "I": "int", "J": "long", "F": "float", "D": "double", "V": "void"}


def desc_types(desc):
    """'(Ljava/lang/String;J)Z' → (['java.lang.String', 'long'], 'boolean')"""
    def one(s, i):
        dims = 0
        while s[i] == "[":
            dims += 1; i += 1
        if s[i] == "L":
            j = s.index(";", i); t = s[i + 1:j].replace("/", "."); i = j + 1
        else:
            t = PRIM[s[i]]; i += 1
        return t + "[]" * dims, i
    args, i = [], 1
    while desc[i] != ")":
        t, i = one(desc, i); args.append(t)
    ret, _ = one(desc, i + 1)
    return args, ret


def keep_rules(refs, cpp_dir):
    lines = ["# GENERATED by scripts/r8-jni.py from the C++ in %s — do not edit; regenerated on every release build." % cpp_dir,
             "# What the native server reaches by name. Classes this app does not compile only add an R8 'unused rule' note."]
    for cls in sorted(refs):
        r = refs[cls]
        lines.append("-keep class %s {" % cls)
        for meth in sorted(r["natives"]):
            lines.append("    native *** %s(...);" % meth)
        for name, sig, static in sorted(r["calls"]):
            args, ret = desc_types(sig)
            lines.append("    %s%s %s(%s);" % ("static " if static else "", ret, name, ", ".join(args)))
        for name, sig, static in sorted(r["fields"]):
            t, _ = desc_types("(" + sig + ")V")[0][0], None
            lines.append("    %s%s %s;" % ("static " if static else "", t, name))
        lines.append("}")
    return "\n".join(lines) + "\n"


# ── dex side (a minimal reader: class_defs → class_data → method names + descriptors) ───────────────────
def uleb(b, o):
    r = s = 0
    while True:
        x = b[o]; o += 1; r |= (x & 0x7F) << s; s += 7
        if x < 0x80:
            return r, o


def read_dex(b, into):
    """into[class] = {(name, descriptor, static)} for every method DEFINED in the dex (not merely referenced)."""
    (sid_n, sid_o, tid_n, tid_o, pid_n, pid_o, fid_n, fid_o, mid_n, mid_o, cd_n, cd_o) = struct.unpack_from("<12I", b, 56)

    def string(i):
        o = struct.unpack_from("<I", b, sid_o + 4 * i)[0]
        _, o = uleb(b, o)
        return b[o:b.index(b"\0", o)].decode("utf-8", "replace")   # MUTF-8; our names are ASCII

    def typ(i):
        return string(struct.unpack_from("<I", b, tid_o + 4 * i)[0])

    def proto(i):
        _, ret, params = struct.unpack_from("<III", b, pid_o + 12 * i)
        ps = []
        if params:
            n = struct.unpack_from("<I", b, params)[0]
            ps = [typ(struct.unpack_from("<H", b, params + 4 + 2 * k)[0]) for k in range(n)]
        return "(" + "".join(ps) + ")" + typ(ret)

    for c in range(cd_n):
        cls_idx, _acc, _sup, _ifs, _src, _ann, data, _sv = struct.unpack_from("<8I", b, cd_o + 32 * c)
        name = typ(cls_idx)[1:-1].replace("/", ".")
        meths = into.setdefault(name, set())
        if not data:
            continue
        o = data
        sf, o = uleb(b, o); inf, o = uleb(b, o); dm, o = uleb(b, o); vm, o = uleb(b, o)
        for _ in range(sf + inf):
            _, o = uleb(b, o); _, o = uleb(b, o)
        for count in (dm, vm):
            idx = 0
            for _ in range(count):
                d, o = uleb(b, o); acc, o = uleb(b, o); _code, o = uleb(b, o)
                idx += d
                _mc, mp, mn = struct.unpack_from("<HHI", b, mid_o + 8 * idx)
                meths.add((string(mn), proto(mp), bool(acc & 0x8)))


def load_dex(paths):
    classes = {}
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if f.endswith(".dex"):
                        with open(os.path.join(root, f), "rb") as fh:
                            read_dex(fh.read(), classes)
        elif p.endswith((".apk", ".aab", ".zip")):
            with zipfile.ZipFile(p) as z:
                for n in z.namelist():
                    if re.search(r"(^|/)classes\d*\.dex$", n):
                        read_dex(z.read(n), classes)
        else:
            with open(p, "rb") as fh:
                read_dex(fh.read(), classes)
    return classes


def declared_classes(src_dirs):
    """Fully-qualified names of top-level classes/objects declared in the given Kotlin/Java source roots."""
    out = set()
    decl = re.compile(r"^\s*(?:(?:public|internal|private|open|abstract|final|data|sealed|annotation|enum)\s+)*"
                      r"(?:class|object|interface)\s+([A-Za-z_][A-Za-z0-9_]*)", re.M)
    for d in src_dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if not f.endswith((".kt", ".java")):
                    continue
                with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
                    src = fh.read()
                pm = re.search(r"^\s*package\s+([\w.]+)", src, re.M)
                pkg = pm.group(1) if pm else ""
                for m in decl.finditer(src):
                    out.add((pkg + "." if pkg else "") + m.group(1))
    return out


def check(refs, dex_paths, src_dirs):
    classes = load_dex(dex_paths)
    if not classes:
        print("!! r8-jni: no dex classes read from %s" % dex_paths); return 1
    declared = declared_classes(src_dirs)
    bad, ok, skipped = [], 0, []
    for cls in sorted(refs):
        r = refs[cls]
        if cls not in declared:
            skipped.append(cls); continue
        if cls not in classes:
            bad.append("class %s is MISSING from the dex (removed or renamed by R8)" % cls); continue
        have = classes[cls]
        names = {n for n, _, _ in have}
        for meth in sorted(r["natives"]):
            if meth in names: ok += 1
            else: bad.append("%s.%s — the native method the C++ exports is missing/renamed" % (cls, meth))
        for name, sig, static in sorted(r["calls"]):
            if (name, sig, static) in have: ok += 1
            else:
                near = sorted("%s%s%s" % ("static " if s else "", n, d) for n, d, s in have if n == name)
                bad.append("%s.%s%s%s — the C++ calls it by name; dex has: %s"
                           % (cls, "static " if static else "", name, sig, near or "nothing by that name"))
    print("r8-jni: %d native/by-name member(s) present in %d class(es)%s"
          % (ok, len(refs) - len(skipped), (" (not compiled into this app: %s)" % ", ".join(skipped)) if skipped else ""))
    for b in bad:
        print("  !! " + b)
    if bad:
        print("!! The C++ reaches these by name and R8 removed, renamed or re-signed them. The keep rules come from "
              "scripts/r8-jni.py — if the C++ looks a class up in a new way, teach scan_cpp() about it.")
        return 1
    print("r8-jni: OK")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["list", "keep", "check"])
    ap.add_argument("--cpp", required=True)
    ap.add_argument("--out")
    ap.add_argument("--src", action="append", default=[])
    ap.add_argument("dex", nargs="*")
    a = ap.parse_intermixed_args()
    refs = scan_cpp(a.cpp)
    if not refs:
        sys.exit("r8-jni: found no JNI references under %s — is that the right directory?" % a.cpp)
    if a.cmd == "list":
        for cls in sorted(refs):
            r = refs[cls]
            print(cls)
            for m in sorted(r["natives"]): print("    native  " + m)
            for n, s, st in sorted(r["calls"]): print("    called  %s%s%s" % ("static " if st else "", n, s))
            for n, s, st in sorted(r["fields"]): print("    field   %s%s %s" % ("static " if st else "", n, s))
        return 0
    if a.cmd == "keep":
        text = keep_rules(refs, os.path.relpath(a.cpp))
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        old = open(a.out).read() if os.path.exists(a.out) else None
        if old != text:
            with open(a.out, "w") as fh:
                fh.write(text)
        return 0
    if not a.dex or not a.src:
        sys.exit("r8-jni check: needs --src and at least one dex/apk/aab")
    return check(refs, a.dex, a.src)


if __name__ == "__main__":
    sys.exit(main())
