#!/bin/bash
# test-r8-jni.sh — self-test for scripts/r8-jni.py (docs/ANDROID-R8.md).
#   ★ With an APK/AAB: the real C++ must PASS against it, and a planted bad reference must FAIL (a check that
#     cannot fail is not a check). Without one: only the C++ scan and the generated rules are tested.
# usage: scripts/test-r8-jni.sh [app-release.apk|.aab]
set -u -o pipefail
cd "$(dirname "$0")/.."
CPP=android/app/src/main/cpp
SRC=android/app/src/main/java
pass=0; fail=0
ok()  { echo "   ok   $1"; pass=$((pass+1)); }
bad() { echo "   FAIL $1"; fail=$((fail+1)); }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

echo "r8-jni: scan + keep rules"
python3 -I scripts/r8-jni.py keep --cpp "$CPP" --out "$T/keep.pro" || bad "keep exited non-zero"
for want in 'com.vibesdr.app.VibeLocalSDR' 'static java.lang.String get(java.lang.String, java.lang.String);' \
            'static boolean download(java.lang.String, java.lang.String, long);' 'fetchIf('; do
  c=$(grep -cF -- "$want" "$T/keep.pro" || true)
  [ "$c" -gt 0 ] && ok "keep rules name: $want" || bad "keep rules lack: $want"
done
# A comment that mentions FindClass must not become a reference; _1 must unmangle to _.
mkdir -p "$T/cpp"
cat > "$T/cpp/t.cpp" <<'EOF'
// FindClass("com/vibesdr/app/OnlyInAComment")
static const char* u = "http://example//not-a-comment";
void f(JNIEnv* env) { jclass c = env->FindClass("com/vibesdr/app/VibeHttp");
  env->GetStaticMethodID(c, "get", "(Ljava/lang/String;)Ljava/lang/String;"); }
extern "C" JNIEXPORT void JNICALL Java_com_vibesdr_app_VibeMapGL_noSuch_1thing(JNIEnv*, jclass) {}
EOF
L=$(python3 -I scripts/r8-jni.py list --cpp "$T/cpp")
c=$(grep -c OnlyInAComment <<<"$L" || true); [ "$c" -eq 0 ] && ok "comments ignored" || bad "a comment became a reference"
c=$(grep -c 'noSuch_thing' <<<"$L" || true);  [ "$c" -eq 1 ] && ok "_1 unmangles to _" || bad "JNI unmangling"

if [ $# -ge 1 ]; then
  echo "r8-jni: check against $1"
  python3 -I scripts/r8-jni.py check --cpp "$CPP" --src "$SRC" "$1" >/dev/null && ok "real C++ passes" || bad "real C++ FAILS against $1"
  out=$(python3 -I scripts/r8-jni.py check --cpp "$T/cpp" --src "$SRC" "$1"); rc=$?
  [ $rc -ne 0 ] && ok "planted bad reference fails" || bad "planted bad reference PASSED — the check is blind"
  c=$(grep -c 'static get(Ljava/lang/String;)' <<<"$out" || true); [ "$c" -gt 0 ] && ok "wrong signature reported" || bad "wrong signature not reported"
  c=$(grep -c 'noSuch_thing' <<<"$out" || true);                   [ "$c" -gt 0 ] && ok "missing native reported" || bad "missing native not reported"
fi
echo "r8-jni: $pass passed, $fail failed"
[ $fail -eq 0 ]
