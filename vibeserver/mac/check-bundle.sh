#!/usr/bin/env bash
# THE MINIMUM-macOS GATE — every Mach-O we ship, every archive we link, every linker warning.
#
# ★★★ WHY THIS EXISTS. 5.6.78 went out notarised and "working" with LSMinimumSystemVersion 14.0 in
#     its Info.plist, and:
#       - vibeserver-engine (the binary Full mode spawns) at minos 27.0 — CMake set no deployment
#         target, so it inherited the BUILD MAC's OS, a macOS 27 beta. Full mode could not start on
#         any real user's Mac.
#       - cloudflared at minos 15.0 (Cloudflare's own release binary): no tunnel on macOS 14.
#       - librtlsdr/libusb/libopus/libhackrf taken from Homebrew bottles built for macOS 26, linked
#         into a 14.0 app. ld stamps the FINAL binary with the minos it was asked for, so otool on
#         the app looked fine — but the code inside may call APIs 14 does not have. The only trace
#         was an ld warning ("built for newer macOS version (26.0) than being linked (14.0)") that
#         a `>/dev/null` swallowed.
#     Nothing about that fails on the machine that built it. It fails on a user's Mac, and the
#     user has to tell you (Stuart, 2026-09-28: "we had a problem like this in the past where a
#     driver wasnt correctly bundled ... and a user had to point it out to me").
# ★★ So it is a GATE, not a report: any row that fails fails the build. build-app.sh runs it at the
#    end with everything it knows (archives, linker logs, link inputs); notarise-and-release.sh
#    runs it again on the bundle alone, before signing, so a hand-copied or stale app cannot slip
#    through either.
#
# Usage:
#   check-bundle.sh --app VibeServer.app [--min 14.0]
#                   [--archive lib.a]...     static archives that were linked: every object's minos
#                   [--ldlog build.log]...   captured build output: "built for newer" / "only available on macOS" = FAIL
#                   [--linkinput file]...    link commands: any Homebrew/MacPorts/usr-local lib = FAIL
#   check-bundle.sh --min 14.0 --archive lib.a...     (build-deps.sh: archives only, no app)
# --min defaults to the app's LSMinimumSystemVersion — what users are PROMISED is what we check.
set -euo pipefail

APP="" MIN=""
ARCHIVES=() LDLOGS=() LINKINPUTS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --app) APP="$2"; shift 2 ;;
    --min) MIN="$2"; shift 2 ;;
    --archive) ARCHIVES+=("$2"); shift 2 ;;
    --ldlog) LDLOGS+=("$2"); shift 2 ;;
    --linkinput) LINKINPUTS+=("$2"); shift 2 ;;
    *) echo "check-bundle: unknown argument $1" >&2; exit 2 ;;
  esac
done

if [ -n "$APP" ]; then
  [ -d "$APP" ] || { echo "!! check-bundle: no app at $APP"; exit 1; }
  PLIST_MIN=$(/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$APP/Contents/Info.plist" 2>/dev/null || true)
  [ -n "$PLIST_MIN" ] || { echo "!! check-bundle: $APP has no LSMinimumSystemVersion"; exit 1; }
  if [ -n "$MIN" ] && [ "$MIN" != "$PLIST_MIN" ]; then
    echo "!! check-bundle: --min $MIN disagrees with the app's LSMinimumSystemVersion $PLIST_MIN"; exit 1
  fi
  MIN="$PLIST_MIN"
fi
[ -n "$MIN" ] || { echo "!! check-bundle: need --app or --min"; exit 2; }

FAIL=0
ROWS=()
row() { ROWS+=("$(printf '%-8s %-52s %-7s %-7s %s' "$1" "$2" "$3" "$4" "$5")"); }
# ver_gt A B → true when version A > version B (numeric, component-wise; 14.0 == 14 == 14.0.0).
ver_gt() { awk -v a="$1" -v b="$2" 'BEGIN{na=split(a,x,".");nb=split(b,y,".");n=na>nb?na:nb;
  for(i=1;i<=n;i++){p=(i<=na)?x[i]+0:0;q=(i<=nb)?y[i]+0:0;if(p>q)exit 0;if(p<q)exit 1}exit 1}'; }
short() { local p="$1"; [ -n "$APP" ] && p="${p#"$(dirname "$APP")/"}"; echo "$p"; }

# ── 1. Every Mach-O in the bundle ────────────────────────────────────────────────────────────
# ★ By CONTENT, not by location or name: a Mach-O dropped in Resources/ or Frameworks/ is shipped
#   code all the same, and the next helper somebody adds will not be where this script expects.
if [ -n "$APP" ]; then
  NMACHO=0
  while IFS= read -r -d '' f; do
    file -b "$f" | grep -q '^Mach-O' || continue
    NMACHO=$((NMACHO+1))
    for arch in $(lipo -archs "$f" 2>/dev/null); do
      L=$(otool -arch "$arch" -l "$f")
      # minos: LC_BUILD_VERSION (platform 1 = macOS) or the older LC_VERSION_MIN_MACOSX.
      read -r PLAT MINOS < <(echo "$L" | awk '
        /cmd LC_BUILD_VERSION/{bv=1;next} /cmd LC_VERSION_MIN_MACOSX/{vm=1;next}
        bv&&$1=="platform"{p=$2} bv&&$1=="minos"{m=$2;bv=0}
        vm&&$1=="version"{p=1;m=$2;vm=0}
        END{print (p==""?"?":p), (m==""?"?":m)}')
      PROB=""
      if [ "$MINOS" = "?" ]; then PROB="; NO minimum-OS load command"
      elif [ "$PLAT" != "1" ]; then PROB="; platform $PLAT is not macOS"
      elif ver_gt "$MINOS" "$MIN"; then PROB="; minos $MINOS > $MIN"; fi
      # Linked dylibs: the load commands themselves (LC_ID_DYLIB, a dylib's own name, is not one).
      DIR=$(dirname "$f")
      RPATHS=$(echo "$L" | awk '/cmd LC_RPATH/{r=1;next} r&&$1=="path"{print $2;r=0}')
      DEPS=$(echo "$L" | awk '/cmd LC_(LOAD|LOAD_WEAK|REEXPORT|LAZY_LOAD|LOAD_UPWARD)_DYLIB$/{d=1;next}
                              d&&$1=="name"{print $2;d=0}')
      NDEP=0
      for d in $DEPS; do
        NDEP=$((NDEP+1))
        case "$d" in
          /usr/lib/*|/System/Library/*) continue ;;
        esac
        # ★ @-relative is fine ONLY if it lands inside this bundle; anything else is a file the
        #   user's Mac will not have (the 0.2.0 "Library not loaded: librtlsdr.0.dylib" crash).
        RES=""
        case "$d" in
          @executable_path/*) RES="$(dirname "$APP/Contents/MacOS/x")/${d#@executable_path/}" ;;
          @loader_path/*)     RES="$DIR/${d#@loader_path/}" ;;
          @rpath/*)
            for rp in $RPATHS; do
              rp="${rp/#@executable_path/$APP/Contents/MacOS}"; rp="${rp/#@loader_path/$DIR}"
              [ -e "$rp/${d#@rpath/}" ] && { RES="$rp/${d#@rpath/}"; break; }
            done ;;
        esac
        if [ -n "$RES" ] && [ -e "$RES" ]; then
          REAL=$(cd "$(dirname "$RES")" && pwd -P)/$(basename "$RES")
          APPREAL=$(cd "$APP" && pwd -P)
          case "$REAL" in "$APPREAL"/*) continue ;; esac
        fi
        PROB="$PROB; links $d (not system, not in bundle)"
      done
      if [ -n "$PROB" ]; then
        FAIL=1; row "Mach-O" "$(short "$f")" "$arch" "$MINOS" "FAIL${PROB#;}"
      else
        row "Mach-O" "$(short "$f")" "$arch" "$MINOS" "PASS $NDEP dylibs, all system or in-bundle"
      fi
    done
  done < <(find "$APP" -type f -print0)
  [ "$NMACHO" -gt 0 ] || { row "Mach-O" "(none found)" "-" "-" "FAIL no executables in the bundle"; FAIL=1; }
fi

# ── 2. Every object inside every static archive that was linked ──────────────────────────────
# ★★ ld stamps the FINAL binary with the minos it was asked for, whatever its inputs were built
#    for — so section 1 cannot see a Homebrew bottle object built for 26 inside a "14.0" binary.
#    That code may still call an API 14 does not have. Only the archive itself tells the truth.
for a in ${ARCHIVES[@]+"${ARCHIVES[@]}"}; do
  if [ ! -f "$a" ]; then row "archive" "$(basename "$a")" "-" "-" "FAIL missing"; FAIL=1; continue; fi
  read -r NOBJ MAXV NOVER NHIGH FIRSTHIGH < <(otool -l "$a" | awk -v min="$MIN" '
    function gt(a,b,  x,y,na,nb,n,i,p,q){na=split(a,x,".");nb=split(b,y,".");n=na>nb?na:nb;
      for(i=1;i<=n;i++){p=(i<=na)?x[i]+0:0;q=(i<=nb)?y[i]+0:0;if(p>q)return 1;if(p<q)return 0}return 0}
    function flush(){ if(obj!=""){ n++; if(v==""){nov++} else { if(max==""||gt(v,max))max=v;
                       if(gt(v,min)){hi++; if(fh=="")fh=obj} } } obj="";v="" }
    /^Archive :/{next}
    /\(.*\):$/{flush(); obj=$0; sub(/^.*\(/,"",obj); sub(/\):$/,"",obj); next}
    /cmd LC_BUILD_VERSION/{bv=1;next} /cmd LC_VERSION_MIN_MACOSX/{vm=1;next}
    bv&&$1=="minos"{v=$2;bv=0} vm&&$1=="version"{v=$2;vm=0}
    END{flush(); print n+0, (max==""?"-":max), nov+0, hi+0, (fh==""?"-":fh)}')
  NOTE="$NOBJ objects"
  [ "$NOVER" -gt 0 ] && NOTE="$NOTE, $NOVER with no version stamp"
  if [ "$NOBJ" -eq 0 ]; then NOTE="no objects"; FAIL=1; ST=FAIL
  elif [ "$NHIGH" -gt 0 ]; then NOTE="$NOTE, $NHIGH above $MIN (e.g. $FIRSTHIGH)"; FAIL=1; ST=FAIL
  else ST=PASS; fi
  row "archive" "$(basename "$a")" "-" "$MAXV" "$ST $NOTE"
done

# ── 3. The compiler's and linker's own warnings ──────────────────────────────────────────────
# ★ Belt and braces for section 2: an input we did not think to list (a -l flag, an object from
#   somewhere else) still makes ld say so. Captured, not thrown at /dev/null.
# ★ And clang's availability warnings: compiled against a NEWER SDK than the minimum, a call to an
#   API the minimum lacks compiles fine and only warns ("is only available on macOS 15.0 or
#   newer") — then crashes on 14 with a missing symbol. That is the same fault as a high minos.
for l in ${LDLOGS[@]+"${LDLOGS[@]}"}; do
  [ -f "$l" ] || { row "ld log" "$(basename "$l")" "-" "-" "FAIL missing"; FAIL=1; continue; }
  N=$(grep -ciE 'built for newer|linking in (object|dylib) file .* built for|only available on macOS|unguarded-availability' "$l" || true)
  if [ "$N" -gt 0 ]; then
    row "ld log" "$(basename "$l")" "-" "-" "FAIL $N newer-macOS warnings (ld minos / API availability)"; FAIL=1
    grep -iE 'built for newer|linking in (object|dylib) file .* built for|only available on macOS|unguarded-availability' "$l" | head -5 | sed 's/^/           /' >&2
  else
    row "ld log" "$(basename "$l")" "-" "-" "PASS no newer-macOS warnings (ld minos / API availability)"
  fi
done

# ── 4. What went INTO the link: nothing from a package manager ───────────────────────────────
# ★ A Homebrew archive is built for the Homebrew user's OS, and a Homebrew dylib is absent on
#   every other Mac. Either way it must never reach a link line again.
for i in ${LINKINPUTS[@]+"${LINKINPUTS[@]}"}; do
  [ -f "$i" ] || { row "link in" "$(basename "$i")" "-" "-" "FAIL missing"; FAIL=1; continue; }
  N=$(grep -oE '(/opt/homebrew|/usr/local/(lib|opt|Cellar)|/opt/local)/[^ "]*' "$i" | sort -u || true)
  if [ -n "$N" ]; then
    row "link in" "$(short "$i")" "-" "-" "FAIL package-manager paths: $(echo $N)"; FAIL=1
  else
    row "link in" "$(short "$i")" "-" "-" "PASS no Homebrew/MacPorts/usr-local libraries"
  fi
done

echo "==> minimum-macOS gate (target: macOS $MIN)"
printf '    %-8s %-52s %-7s %-7s %s\n' "KIND" "ITEM" "ARCH" "MINOS" "RESULT"
for r in ${ROWS[@]+"${ROWS[@]}"}; do echo "    $r"; done
if [ "$FAIL" != 0 ]; then
  echo "!! GATE FAILED — something in this build will not run on macOS $MIN (see FAIL rows)."
  exit 1
fi
echo "==> gate PASSED: everything checked runs on macOS $MIN"
