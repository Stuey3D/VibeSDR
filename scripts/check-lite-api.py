#!/usr/bin/env python3
"""check-lite-api.py — no React Native module in an APK may NAME a framework class newer than the APK's minSdk.

★★★ WHY (Kiko, 2026-10-14). Lite 65 crashed at launch on an Android 6 (API 23) Amlogic box:
      Exception in HostObject::get for prop 'VibeLocalSDR': java.lang.NoClassDefFoundError: android.view.FrameMetrics
    VibeLocalSdrModule.kt is the MAIN app's file, compiled into Lite as-is (lite/android/app/build.gradle, SHARED).
    The frame-rate AUTO work (257daa12) put an API-24 listener in it, correctly behind SDK_INT checks — but its lambda
    was desugared into a static method OF THE MODULE CLASS with FrameMetrics in the signature. React Native finds
    @ReactMethods with getDeclaredMethods(), which resolves EVERY declared method's parameter and return types, so the
    whole module failed to load before any check could run. The app's minSdk is 24, so it never showed there; Lite's
    is 21.
 ★ The rule this enforces: in a class that extends a React bridge module, no method or field signature may name an
   android.* class whose "since" level is above minSdk. Such code goes in its own @RequiresApi class (IcuLatin,
   FrameWatchN in VibeLocalSdrModule.kt). Other classes are not checked: ART resolves their types lazily, and a call
   behind an SDK_INT check is safe there (VibeExitInfo has run on API 23 since 2026-10-07).
 ★ Data: the SDK's own platforms/android-N/data/api-versions.xml (the table lint uses) and build-tools' dexdump.

usage: check-lite-api.py <apk> <minSdk> <api-versions.xml> <dexdump>      exit 1 = a module names a too-new class
"""
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

MODULE_BASES = {"Lcom/facebook/react/bridge/ReactContextBaseJavaModule;", "Lcom/facebook/react/bridge/BaseJavaModule;"}


def main():
    apk, minsdk, apixml, dexdump = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    since = {c.get("name"): int(c.get("since", "1")) for c in ET.parse(apixml).getroot().iter("class")}
    supers, sigs = {}, {}                   # class → superclass; class → [(member, type descriptor)]
    with tempfile.TemporaryDirectory() as d, zipfile.ZipFile(apk) as z:
        for n in z.namelist():
            if not re.match(r"classes\d*\.dex$", n):
                continue
            out = subprocess.run([dexdump, z.extract(n, d)], capture_output=True, text=True, errors="replace").stdout
            cls, name = None, None
            for line in out.splitlines():
                m = re.match(r"\s*Class descriptor\s*:\s*'(L[^']+;)'", line)
                if m:
                    cls = m.group(1); sigs.setdefault(cls, []); continue
                if cls is None:
                    continue
                m = re.match(r"\s*Superclass\s*:\s*'(L[^']+;)'", line)
                if m:
                    supers[cls] = m.group(1); continue
                m = re.match(r"\s*name\s*:\s*'([^']*)'", line)
                if m:
                    name = m.group(1); continue
                m = re.match(r"\s*type\s*:\s*'([^']+)'", line)
                if m:
                    sigs[cls].append((name, m.group(1)))

    def is_module(c):
        seen = set()
        while c in supers and c not in seen:
            seen.add(c)
            c = supers[c]
            if c in MODULE_BASES:
                return True
        return False

    modules = [c for c in sigs if is_module(c)]
    bad = []
    for c in modules:
        for member, desc in sigs[c]:
            for t in re.findall(r"L(android/[^;]+);", desc):
                s = since.get(t)
                if s and s > minsdk:
                    bad.append((s, c, member, t))
    print("check-lite-api: %d React module class(es) checked against minSdk %d" % (len(modules), minsdk))
    for s, c, member, t in sorted(set(bad), reverse=True):
        print("  !! API %d  %s  in %s.%s" % (s, t.replace("/", "."), c[1:-1].replace("/", "."), member))
    if bad:
        print("!! A React module names a class newer than minSdk — React Native's getDeclaredMethods() will throw "
              "NoClassDefFoundError on older Android and the module never loads. Move that code into its own "
              "@RequiresApi class (see FrameWatchN / IcuLatin in VibeLocalSdrModule.kt).")
        return 1
    if not modules:
        print("!! no React module classes found — the check is not seeing the APK it should")
        return 1
    print("check-lite-api: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
