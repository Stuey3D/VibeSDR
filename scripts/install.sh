#!/usr/bin/env bash
# VibeServer — one-line installer.  Served as https://apt.vibesdr.net/install.sh
#
#   curl -fsSL https://apt.vibesdr.net/install.sh | sudo bash
#
# Run it again at any time: it upgrades to the newest VibeServer and changes nothing else.
#
# Options (after `bash -s --` when piped, or as environment variables, which pass through sudo):
#   --auto-updates      also install VibeServer updates automatically    VIBE_AUTO_UPDATES=1
#   --no-auto-updates   turn that off again                              VIBE_AUTO_UPDATES=0
#   --dry-run           say what would be done, change nothing           VIBE_DRY_RUN=1
#   --yes               macOS: install without asking                    VIBE_YES=1
#   e.g.  curl -fsSL https://apt.vibesdr.net/install.sh | sudo VIBE_AUTO_UPDATES=1 bash
#
# Exit codes:
#   0 done (installed, upgraded, already current, or a dry run)
#   1 something failed unexpectedly            2 bad option
#   3 this operating system is not supported   4 this processor is not supported
#   5 wrong privileges (Linux needs sudo; on a Mac, run it WITHOUT sudo)
#   6 a download or the signing-key check failed
#   7 apt failed                               8 nothing done: the person said no, or no terminal to ask on
#
# ★★★ ONE SCRIPT, EVERY VERSION (Stuart, 2026-10-18: "I would like every version to be behind the same
#     one script install and the right version installs and updates if possible"). apt already picks
#     the right package per architecture; this script picks the right PLATFORM, sets apt up exactly as
#     the manual steps on apt.vibesdr.net do, and from then on apt IS the update path — the admin
#     page's Install updates button and `apt upgrade` both use the repository this sets up.
# ★★ THE WHOLE SCRIPT IS ONE FUNCTION, CALLED ON THE LAST LINE. Piped into bash, a download cut off
#    halfway would otherwise run half a script. Nothing runs until the closing line has arrived.
# ★★ stdin IS THIS SCRIPT when piped, so every apt/dpkg call reads </dev/null — a command that reads
#    stdin would swallow the rest of the file — and questions go to /dev/tty.
# ★★ BASH 3.2 — macOS still ships it. No associative arrays, no ${x,,}, no mapfile, no empty "${a[@]}".
# ★★ NEVER `cmd | grep -q` under pipefail: grep -q exits at the first match, the writer takes SIGPIPE,
#    and a SUCCESSFUL match is reported as a failure. grep a FILE, or count with `grep -c … || true`.

vibe_install_main() {
set -euo pipefail

REPO_URL="https://apt.vibesdr.net"
KEY_URL="$REPO_URL/KEY.gpg"
KEY_FPR="2B54962707D8119929E1FB2BFC384096D89F0AE7"   # ed25519/FC384096D89F0AE7, VibeSDR Package Signing
KEYRING="/usr/share/keyrings/vibesdr.gpg"
LIST="/etc/apt/sources.list.d/vibesdr.list"
UU_CONF="/etc/apt/apt.conf.d/52vibesdr-auto-updates"
# The Release file's Origin and Label (aptftp.conf in the apt repository) — what unattended-upgrades matches.
UU_PATTERN="origin=VibeSDR,label=VibeSDR,site=apt.vibesdr.net"
MAC_REPO="Stuey3D/VibeSDR"
MAC_TEAM="6PV2X6THHM"
MIN_GLIBC="2.35"                                      # every published package Depends: libc6 (>= 2.35)
# ★ For the test matrix only: a fake /proc/cpuinfo, so the no-SSE2 refusal can be exercised on a
#   machine that has SSE2. Nothing else reads it.
CPUINFO="${VIBE_CPUINFO:-/proc/cpuinfo}"

AUTO="${VIBE_AUTO_UPDATES:-}"
DRY="${VIBE_DRY_RUN:-0}"
YES="${VIBE_YES:-0}"
while [ $# -gt 0 ]; do
  case "$1" in
    --auto-updates)    AUTO=1 ;;
    --no-auto-updates) AUTO=0 ;;
    --dry-run)         DRY=1 ;;
    --yes|-y)          YES=1 ;;
    -h|--help)
      cat <<'HELP'
VibeServer installer:  curl -fsSL https://apt.vibesdr.net/install.sh | sudo bash
  --auto-updates      also install VibeServer updates automatically  (VIBE_AUTO_UPDATES=1)
  --no-auto-updates   turn that off again                            (VIBE_AUTO_UPDATES=0)
  --dry-run           say what would be done, change nothing         (VIBE_DRY_RUN=1)
  --yes               macOS: install without asking                  (VIBE_YES=1)
Options go after `bash -s --` when piped; the VIBE_* forms pass through sudo:
  curl -fsSL https://apt.vibesdr.net/install.sh | sudo VIBE_AUTO_UPDATES=1 bash
HELP
      return 0 ;;
    *) echo "VibeServer installer: unknown option '$1' (try --help)." >&2; return 2 ;;
  esac
  shift
done
case "$AUTO" in ""|0|1) ;; yes|true|on) AUTO=1 ;; no|false|off) AUTO=0 ;;
  *) echo "VibeServer installer: VIBE_AUTO_UPDATES must be 1 or 0." >&2; return 2 ;; esac
case "$DRY" in 1|yes|true) DRY=1 ;; *) DRY=0 ;; esac
case "$YES" in 1|yes|true) YES=1 ;; *) YES=0 ;; esac

TMP=""
cleanup() { [ -n "$TMP" ] && rm -rf "$TMP"; return 0; }
trap cleanup EXIT

say()  { printf '%s\n' "$*"; }
step() { printf '==> %s\n' "$*"; }
warn() { printf 'Note: %s\n' "$*" >&2; }
die()  { local code="$1"; shift; printf 'VibeServer installer: %s\n' "$*" >&2; exit "$code"; }
# Run a command, or in a dry run only say it.
doit() { if [ "$DRY" = 1 ]; then say "   (dry run) would run: $*"; else "$@"; fi; }

# ───────────────────────────────────────────────────────────────────────────────────────────────
#  macOS — the notarised menu-bar app, never apt
# ───────────────────────────────────────────────────────────────────────────────────────────────
mac_install() {
  say "This is a Mac. VibeServer for macOS is a signed, notarised menu-bar app (Apple Silicon,"
  say "macOS 14 or later) rather than an apt package. It is published at"
  say "  https://github.com/$MAC_REPO/releases"
  say ""
  if [ "$(id -u)" = 0 ]; then
    say "This script was run with sudo. On a Mac, run it WITHOUT sudo and it can download the app"
    say "into Applications for you:"
    say "  curl -fsSL $REPO_URL/install.sh | bash"
    exit 5
  fi
  if [ "$(uname -m)" != "arm64" ]; then
    # ★ Rosetta reports x86_64 for a translated shell on Apple Silicon — sysctl tells the truth.
    if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != 1 ]; then
      die 4 "this Mac has an Intel processor; the macOS app is built for Apple Silicon only. A Linux machine or a Raspberry Pi can run VibeServer instead — see $REPO_URL"
    fi
  fi
  local osmajor; osmajor="$(sw_vers -productVersion 2>/dev/null | cut -d. -f1)"
  if [ -n "$osmajor" ] && [ "$osmajor" -lt 14 ] 2>/dev/null; then
    die 3 "VibeServer for macOS needs macOS 14 (Sonoma) or later; this Mac has $(sw_vers -productVersion)."
  fi

  TMP="$(mktemp -d "${TMPDIR:-/tmp}/vibeserver-install.XXXXXX")"
  step "Looking up the latest VibeServer for macOS"
  curl -fsSL -H 'Accept: application/vnd.github+json' \
       "https://api.github.com/repos/$MAC_REPO/releases?per_page=50" -o "$TMP/releases.json" \
    || die 6 "could not reach GitHub to find the latest release. Download it by hand from https://github.com/$MAC_REPO/releases"
  # ★ JSON parsed with JavaScript for Automation, which every Mac has — python3 is not installed by
  #   default any more. Newest first is GitHub's order; drafts never appear, pre-releases are skipped.
  # ★ Written to a file first: a here-document inside $( ) is fragile in bash 3.2.
  cat > "$TMP/pick.js" <<'JXA'
function run(argv) {
  var s = $.NSString.stringWithContentsOfFileEncodingError(argv[0], $.NSUTF8StringEncoding, null).js;
  var rel = JSON.parse(s).find(function (r) {
    return !r.draft && !r.prerelease && /^vibeserver-mac-v\d+\.\d+\.\d+$/.test(r.tag_name); });
  if (!rel) return "";
  var a = rel.assets.find(function (x) { return /^VibeServer-[0-9.]+\.zip$/.test(x.name); });
  if (!a) return "";
  return [rel.tag_name.replace("vibeserver-mac-v", ""), a.browser_download_url,
          (a.digest || "").replace(/^sha256:/, ""), a.name].join("\t");
}
JXA
  local info
  info="$(/usr/bin/osascript -l JavaScript "$TMP/pick.js" "$TMP/releases.json" 2>/dev/null || true)"
  [ -n "$info" ] || die 6 "could not find a macOS release on GitHub. Download it by hand from https://github.com/$MAC_REPO/releases"
  local ver url sha name
  ver="$(printf '%s' "$info" | cut -f1)"; url="$(printf '%s' "$info" | cut -f2)"
  sha="$(printf '%s' "$info" | cut -f3)"; name="$(printf '%s' "$info" | cut -f4)"
  case "$url" in "https://github.com/$MAC_REPO/releases/download/"*) ;;
    *) die 6 "the release points somewhere unexpected ($url) — not downloading it." ;; esac

  local app="/Applications/VibeServer.app" have=""
  if [ -d "$app" ]; then
    have="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist" 2>/dev/null || true)"
  fi
  if [ -n "$have" ]; then say "Installed: VibeServer $have.   Latest: $ver."; else say "Latest: VibeServer $ver."; fi
  if [ "$have" = "$ver" ]; then
    say "You already have the latest version. Updates for the Mac app come from the same page."
    # ★ A dry run carries on and checks the download and its signature anyway — that is its point.
    [ "$DRY" = 1 ] || return 0
    say "(dry run) checking the download and its signature anyway."
  fi

  if [ "$DRY" != 1 ] && [ "$YES" != 1 ]; then
    if ( exec </dev/tty ) 2>/dev/null; then
      local ans=""
      printf 'Download VibeServer %s and install it in Applications? [y/N] ' "$ver" >/dev/tty
      read -r ans </dev/tty || ans=""
      case "$ans" in y|Y|yes|YES|Yes) ;; *) say "Nothing changed."; exit 8 ;; esac
    else
      say "No terminal to ask on. To install without being asked:"
      say "  curl -fsSL $REPO_URL/install.sh | VIBE_YES=1 bash"
      exit 8
    fi
  fi

  step "Downloading $name"
  curl -fL --progress-bar "$url" -o "$TMP/$name" || die 6 "the download failed. Try again, or download it by hand from https://github.com/$MAC_REPO/releases"
  if [ -n "$sha" ]; then
    local got; got="$(shasum -a 256 "$TMP/$name" | awk '{print $1}')"
    [ "$got" = "$sha" ] || die 6 "the download does not match the checksum GitHub publishes for it — not installing it."
    say "   checksum matches GitHub's ($sha)"
  fi
  mkdir "$TMP/x"
  ditto -x -k "$TMP/$name" "$TMP/x" || die 6 "could not unpack $name."
  local new="$TMP/x/VibeServer.app"
  [ -d "$new" ] || die 6 "$name does not contain VibeServer.app — not installing it."

  # ★★★ VERIFY BEFORE ANYTHING IN /Applications IS TOUCHED: the signature is intact, it is OUR team's,
  #     and Gatekeeper accepts it as notarised. A failure here leaves the installed app exactly as it was.
  step "Checking the signature"
  codesign --verify --deep --strict "$new" 2>/dev/null || die 6 "the app's code signature does not verify — not installing it."
  local team; team="$(codesign -dv "$new" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
  [ "$team" = "$MAC_TEAM" ] || die 6 "the app is signed by team '$team', not VibeSDR's ($MAC_TEAM) — not installing it."
  local assess; assess="$(spctl -a -vv "$new" 2>&1 || true)"
  case "$assess" in *": accepted"*"source=Notarized Developer ID"*) ;;
    *) die 6 "Gatekeeper does not accept the app as notarised — not installing it. ($assess)" ;; esac
  local newver; newver="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$new/Contents/Info.plist" 2>/dev/null || true)"
  [ "$newver" = "$ver" ] || die 6 "the app inside says version '$newver', not $ver — not installing it."
  say "   signed by $MAC_TEAM, notarised, version $newver"

  if [ "$DRY" = 1 ]; then
    say "(dry run) Verified and ready. Would install it as $app${have:+, moving $have to the Bin}."
    return 0
  fi
  if pgrep -f "$app/Contents/MacOS/" >/dev/null 2>&1; then
    die 8 "VibeServer is running. Quit it from its menu-bar icon (anyone listening will be disconnected), then run this again."
  fi
  [ -w /Applications ] || die 5 "this account cannot write to /Applications. Sign in as an administrator, or drag VibeServer.app there from the download."

  step "Installing"
  local old=""
  if [ -e "$app" ]; then
    old="$TMP/VibeServer.previous.app"
    mv "$app" "$old" || die 1 "could not move the old app aside; nothing changed."
  fi
  if ! ditto "$new" "$app" || ! codesign --verify --deep --strict "$app" 2>/dev/null; then
    rm -rf "$app"
    [ -n "$old" ] && mv "$old" "$app"
    die 1 "could not install the new app; the previous one has been put back."
  fi
  if [ -n "$old" ]; then
    # ★ To the Bin, not deleted: recoverable if anything about the new build is wrong.
    local bin="$HOME/.Trash/VibeServer $have.app"
    [ -e "$bin" ] && bin="$HOME/.Trash/VibeServer $have $(date +%Y%m%d%H%M%S).app"
    mv "$old" "$bin" 2>/dev/null || rm -rf "$old"
  fi
  say ""
  say "VibeServer $ver is installed in Applications. Open it, and it appears in the menu bar;"
  say "choose Open in Browser to set it up."
  return 0
}

# ───────────────────────────────────────────────────────────────────────────────────────────────
#  Platforms we cannot serve — said plainly
# ───────────────────────────────────────────────────────────────────────────────────────────────
windows_explain() {
  say "VibeServer has no Windows version yet, and it cannot be installed from here."
  say "Windows Subsystem for Linux is not supported either: it cannot see a USB radio without"
  say "extra tools, and a server inside it is not reachable from the rest of your network."
  say "What works today: a Raspberry Pi or any Debian/Ubuntu machine ($REPO_URL), a Mac with"
  say "Apple Silicon, or an Android phone running the VibeSDR app (Use as server)."
  exit 3
}

case "$(uname -s)" in
  Darwin) mac_install; return 0 ;;
  MINGW*|MSYS*|CYGWIN*) windows_explain ;;
  Linux) ;;
  *) die 3 "VibeServer runs on Debian, Ubuntu and Raspberry Pi OS, and on Apple Silicon Macs; '$(uname -s)' is not one of them." ;;
esac
if grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null || [ -n "${WSL_DISTRO_NAME:-}" ]; then
  windows_explain
fi

# ───────────────────────────────────────────────────────────────────────────────────────────────
#  Linux: which distribution
# ───────────────────────────────────────────────────────────────────────────────────────────────
OS_ID=""; OS_LIKE=""; OS_NAME="this Linux"; OS_VER=""
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  OS_ID="$(. /etc/os-release && printf '%s' "${ID:-}")"
  OS_LIKE="$(. /etc/os-release && printf '%s' "${ID_LIKE:-}")"
  OS_NAME="$(. /etc/os-release && printf '%s' "${PRETTY_NAME:-${NAME:-Linux}}")"
  OS_VER="$(. /etc/os-release && printf '%s' "${VERSION_ID:-}")"
fi
DEBIANISH=0
for w in $OS_ID $OS_LIKE; do
  case "$w" in debian|ubuntu|raspbian) DEBIANISH=1 ;; esac
done
if [ "$DEBIANISH" != 1 ] || ! command -v apt-get >/dev/null 2>&1 || ! command -v dpkg >/dev/null 2>&1; then
  say "VibeServer is packaged for Debian, Ubuntu and Raspberry Pi OS (and distributions built on"
  say "them, such as Linux Mint). This machine runs $OS_NAME, which uses a different package system,"
  say "so there is nothing this script can install here."
  say "VibeServer can be built from source: https://github.com/$MAC_REPO (vibeserver/linux/INSTALL.md)."
  exit 3
fi

# The Portable image updates itself; its root is read-only underneath, so apt here would be lost at reboot.
if [ -f /etc/vibeserver-pocket/enabled ]; then
  say "This is a VibeServer Portable. It updates itself: use Install updates on its admin page."
  say "Nothing changed."
  return 0
fi

if [ "$(id -u)" != 0 ] && [ "$DRY" != 1 ]; then
  say "This needs administrator rights to add the VibeServer repository. Run it with sudo:"
  say "  curl -fsSL $REPO_URL/install.sh | sudo bash"
  exit 5
fi

# ── Architecture ────────────────────────────────────────────────────────────────────────────────
ARCH="$(dpkg --print-architecture)"
cpu_has() { grep -qwE "$1" "$CPUINFO" 2>/dev/null; }
case "$ARCH" in
  amd64|arm64) ;;
  armhf)
    # ★ The armhf package carries two builds: ARMv7+NEON, and a scalar ARMv6 one that its postinst
    #   installs on a CPU without NEON (Pi Zero, Zero W, Pi 1). Nothing to refuse; just say so.
    if ! cpu_has 'neon|asimd'; then
      warn "this processor has no NEON (a Pi Zero, Zero W or Pi 1). The package installs its ARMv6"
      warn "build for it, which works with restrictions: the setup page switches heavy features off."
    fi ;;
  i386)
    # ★★ The same test and the same words as the package's own preinst, so whichever one a person
    #    meets, they read the same thing. /proc may be missing (a chroot): then nothing to read.
    if [ -r "$CPUINFO" ] && grep -q '^flags' "$CPUINFO" && ! grep -qw sse2 "$CPUINFO"; then
      say "VibeServer: this processor has no SSE2, and the 32-bit x86 (i386) build needs it."
      say "  Any Pentium 4, Pentium M, Atom, Athlon 64 or newer has it; a Pentium III or Athlon XP"
      say "  does not. Nothing has been installed."
      exit 4
    fi ;;
  *)
    die 4 "VibeServer is published for 64-bit PCs (amd64), 64-bit ARM (arm64), 32-bit ARM (armhf) and 32-bit PCs (i386); this system is '$ARCH'." ;;
esac

# ── Old enough? Every package needs glibc 2.35 (Debian 12, Raspberry Pi OS bookworm, Ubuntu 22.04) and,
#    since 11.0.5's self-contained build, NOTHING else from the system but curl, gzip and adduser —
#    every radio and codec library is linked in, so 22.04 installs (verified in clean containers,
#    2026-10-18). Packages published before that still fail there on libhackrf0; the apt simulation
#    below catches them.
LIBC="$(dpkg-query -W -f='${Version}' libc6 2>/dev/null || true)"
if [ -n "$LIBC" ] && ! dpkg --compare-versions "$LIBC" ge "$MIN_GLIBC"; then
  die 3 "$OS_NAME is too old for VibeServer: it needs Debian 12, Ubuntu 22.04, Raspberry Pi OS 'bookworm' or newer (C library $MIN_GLIBC; this has ${LIBC%%-*}). Upgrade the system, or install a newer one, and run this again."
fi

say "VibeServer installer — $OS_NAME, $ARCH"
[ "$DRY" = 1 ] && say "(dry run: nothing will be changed)"

export DEBIAN_FRONTEND=noninteractive
APT_OPTS="-y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold"
# ★ apt's own screenful (download meters, "Reading database ... 5%") is kept in a log and shown only
#   if it fails — the person sees what this script is doing, and apt's words when they matter.
# ★ In a private mktemp directory, never a fixed /tmp name: this runs as root, and a fixed name is a
#   symlink away from truncating any file on the machine.
TMP="$(mktemp -d /tmp/vibeserver-install.XXXXXX)"
APT_LOG="$TMP/apt.log"
apt_quiet() {
  if [ "$DRY" = 1 ]; then say "   (dry run) would run: apt-get $*"; return 0; fi
  if apt-get $APT_OPTS -o Dpkg::Use-Pty=0 "$@" </dev/null >>"$APT_LOG" 2>&1; then return 0; fi
  echo "---- the end of apt's output ----" >&2
  tail -n 25 "$APT_LOG" >&2
  echo "----" >&2
  return 1
}
touch "$APT_LOG"

# ── Our repository: one line, in one file ──────────────────────────────────────────────────────
WANT="deb [arch=$ARCH signed-by=$KEYRING] $REPO_URL stable main"
# ★★ DONE FIRST, BEFORE ANY apt update. Another copy of our repository with a different signed-by
#    makes apt refuse to update AT ALL ("Conflicting values set for option Signed-By") — so if it were
#    left until later, installing curl or gpg below would fail on the very thing this script is here
#    to fix. A duplicate gives a warning on every update. Each file touched is backed up first as
#    <file>.vibesdr.bak, which apt ignores silently; a stale vibesdr.list is taken out until the
#    right line is written below.
#  ★ deb822 stanza helpers: is there a stanza naming us that is still enabled; switch such stanzas off.
sources_ours_active() { awk 'BEGIN{RS="";FS="\n"} /apt\.vibesdr\.net/ && !/(^|\n)[Ee]nabled:[[:space:]]*no/ {f=1} END{exit !f}' "$1"; }
sources_disable_ours() { awk 'BEGIN{RS="";ORS="\n\n"} /apt\.vibesdr\.net/ && !/(^|\n)[Ee]nabled:[[:space:]]*no/ {$0=$0"\nEnabled: no"} {print}' "$1"; }
for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  [ -f "$f" ] || continue
  [ "$f" = "$LIST" ] && continue
  # ★★ The admin page's "Receive beta updates" source (suite beta, written by vibeserver-maintenance with the same
  #    signed-by as ours) is the owner's choice, not an older copy: running the installer again must not untick it.
  [ "$f" = "/etc/apt/sources.list.d/vibesdr-beta.list" ] && continue
  # ★ ACTIVE entries only: an entry this script already switched off must not trigger a second
  #   pass, which would overwrite the original backup with the already-disabled copy.
  case "$f" in
    *.sources) sources_ours_active "$f" || continue ;;
    *)         grep -qE '^[[:space:]]*deb(-src)?[[:space:]].*apt\.vibesdr\.net' "$f" || continue ;;
  esac
  say "   switching off an older copy of the VibeServer repository in $f (backup: $f.vibesdr.bak)"
  doit cp -p "$f" "$f.vibesdr.bak"
  case "$f" in
    *.sources)
      # ★ Only OUR stanza is switched off (Enabled: no); a file that also lists other repositories
      #   keeps them working.
      if [ "$DRY" != 1 ]; then sources_disable_ours "$f.vibesdr.bak" > "$f"; fi ;;
    *)
      doit sed -i -E '/apt\.vibesdr\.net/ s|^([[:space:]]*)(deb)|\1# disabled by the VibeServer installer: \2|' "$f" ;;
  esac
done
LIST_WAS=""; NEW_LIST=0; NEW_KEYRING=0
[ -e "$LIST" ] || NEW_LIST=1
if [ -f "$LIST" ]; then
  LIST_WAS="$(grep -vE '^[[:space:]]*(#|$)' "$LIST" 2>/dev/null || true)"
  if [ "$LIST_WAS" != "$WANT" ] || ! [ -s "$KEYRING" ]; then
    say "   taking out the old $LIST (it was: $(printf '%s' "$LIST_WAS" | tr '\n' ' '))"
    doit rm -f "$LIST"
  fi
fi

# ── Tools the script itself needs ──────────────────────────────────────────────────────────────
NEED=""
command -v curl >/dev/null 2>&1 || NEED="$NEED curl"
command -v gpg  >/dev/null 2>&1 || NEED="$NEED gpg"
[ -s /etc/ssl/certs/ca-certificates.crt ] || NEED="$NEED ca-certificates"
if [ -n "$NEED" ]; then
  step "Installing what this needs first:$NEED"
  apt_quiet update || die 7 "apt could not update its package lists. Check the network and 'sudo apt update', then run this again."
  # shellcheck disable=SC2086
  apt_quiet install --no-install-recommends $NEED \
    || die 7 "could not install$NEED. Install them with 'sudo apt install$NEED' and run this again."
fi
if [ "$DRY" = 1 ] && [ -n "$NEED" ]; then
  say "   (dry run) would then check the signing key, add the repository and install VibeServer."
  say ""; say "Dry run finished; nothing was changed."
  return 0
fi

# ── The signing key, checked against the fingerprint written in this script ────────────────────
step "Fetching the signing key"
curl -fsSL "$KEY_URL" -o "$TMP/KEY.gpg" || die 6 "could not download $KEY_URL. Check the network and run this again."
mkdir -m 700 "$TMP/gnupg"
keyinfo() { GNUPGHOME="$TMP/gnupg" gpg --batch --with-colons --show-keys "$1" 2>/dev/null \
            || GNUPGHOME="$TMP/gnupg" gpg --batch --with-colons --import-options show-only --import "$1" 2>/dev/null; }
keyinfo "$TMP/KEY.gpg" > "$TMP/keyinfo" || true
NPUB="$(grep -c '^pub:' "$TMP/keyinfo" || true)"
GOT_FPR="$(awk -F: '$1=="pub"{p=1;next} $1=="fpr"&&p{print $10;exit}' "$TMP/keyinfo")"
if [ "$NPUB" != 1 ] || [ "$GOT_FPR" != "$KEY_FPR" ]; then
  die 6 "the downloaded signing key is not the one this script expects (wanted $KEY_FPR, got '${GOT_FPR:-nothing}'). Nothing has been changed. Please report this: https://github.com/$MAC_REPO/issues"
fi
GNUPGHOME="$TMP/gnupg" gpg --batch --yes --dearmor -o "$TMP/vibesdr.gpg" "$TMP/KEY.gpg"
say "   fingerprint $KEY_FPR — matches"
if [ -f "$KEYRING" ] && cmp -s "$TMP/vibesdr.gpg" "$KEYRING"; then
  :
else
  [ -e "$KEYRING" ] || NEW_KEYRING=1
  doit install -m 0644 -o root -g root "$TMP/vibesdr.gpg" "$KEYRING"
fi

step "Adding the repository"
if [ -f "$LIST" ] && [ "$LIST_WAS" = "$WANT" ]; then
  say "   $LIST is already correct"
elif [ "$DRY" = 1 ]; then
  say "   (dry run) would write $LIST: $WANT"
else
  printf '%s\n' "$WANT" > "$LIST"; chmod 0644 "$LIST"
fi

# ── apt update ─────────────────────────────────────────────────────────────────────────────────
step "Updating package lists"
if [ "$DRY" != 1 ]; then
  if ! apt-get -q update </dev/null >"$TMP/update.log" 2>&1; then
    # ★ Another source failing (a dead PPA) must not stop OUR install — but OUR source must work.
    if apt-get -q update -o Dir::Etc::sourcelist="$LIST" -o Dir::Etc::sourceparts=- \
         -o APT::Get::List-Cleanup=0 </dev/null >"$TMP/update-ours.log" 2>&1; then
      warn "another package source on this machine failed to update (shown below). VibeServer's is fine,"
      warn "so the install carries on, but you may want to fix that source."
      grep -E '^(E|W):' "$TMP/update.log" >&2 || true
    else
      cat "$TMP/update-ours.log" >&2
      die 7 "apt could not read the VibeServer repository. Check the network and run this again."
    fi
  fi
fi

# ── Can this release take it at all? ───────────────────────────────────────────────────────────
# ★★ ASKED BEFORE INSTALLING, so an older release gets a sentence instead of apt's "held broken
#    packages". The C library check above catches most; this catches the rest. Measured 2026-10-18:
#    Ubuntu 22.04 has glibc 2.35 but only libhackrf0 2021.03, and packages up to 11.0.5-1 needed 2022.09
#    or newer. The self-contained build names no library but libc6, so this should now only ever fire
#    for an old package (or a broken apt) — kept, because a sentence beats "held broken packages".
if [ "$DRY" != 1 ] && ! apt-get -s -q install vibeserver </dev/null >"$TMP/sim.log" 2>&1; then
  grep -E '^ *vibeserver *:|Depends:|^E:' "$TMP/sim.log" | sed 's/^ */   /' >&2 || true
  # ★ Leave the machine as it was found: a repository added by THIS run, for a package that cannot
  #   install here, is only a line for apt to fetch on every update. One that was there before stays.
  if [ "$NEW_LIST" = 1 ]; then rm -f "$LIST"; [ "$NEW_KEYRING" = 1 ] && rm -f "$KEYRING"; fi
  die 3 "VibeServer cannot be installed on $OS_NAME: the libraries it needs are older here than the ones it was built against (above). It needs Debian 12, Ubuntu 22.04, Raspberry Pi OS 'bookworm' or newer."
fi

# ── Install or upgrade ─────────────────────────────────────────────────────────────────────────
BEFORE="$(dpkg-query -W -f='${db:Status-Abbrev} ${Version}' vibeserver 2>/dev/null || true)"
case "$BEFORE" in ii*) BEFORE="${BEFORE##* }" ;; *) BEFORE="" ;; esac
if [ "$DRY" = 1 ]; then
  CAND="$(apt-cache policy vibeserver 2>/dev/null | awk '$1=="Candidate:"{print $2}' || true)"
  say "   (dry run) would install vibeserver${CAND:+ $CAND}${BEFORE:+ (installed: $BEFORE)}"
else
  if [ -n "$BEFORE" ]; then step "Upgrading VibeServer (installed: $BEFORE)"
  else step "Installing VibeServer — a few minutes on a Raspberry Pi"; fi
  # ★ `install`, not `upgrade`: it installs if missing and upgrades if present, and touches no other
  #   package beyond what VibeServer itself needs. Recommends follow this machine's own apt setting,
  #   exactly as `sudo apt install vibeserver` would.
  apt_quiet install vibeserver \
    || die 7 "apt could not install VibeServer (its message is above). Fix what it names and run this again."
  # The package's own notes (the ARMv6 build chosen, plugdev, the Pi's USB budget) are worth showing.
  grep -E '^VibeServer( Portable)?:' "$APT_LOG" | grep -v 'run .vibeserver. to set it up' | sed 's/^/   /' || true
fi
AFTER="$(dpkg-query -W -f='${Version}' vibeserver 2>/dev/null || true)"

# ── Automatic updates (opt-in) ─────────────────────────────────────────────────────────────────
# ★★★ OUR REPOSITORY ONLY, NEVER THE WHOLE SYSTEM BY STEALTH. If unattended-upgrades was already
#     running on this machine, the owner chose that: we add our origin to their list and change
#     nothing else. If it was not, the file we write CLEARS the origin lists and names only ours, so
#     turning it on for VibeServer cannot quietly start upgrading the operating system as well. One
#     file holds all of it; --no-auto-updates deletes it and the machine is as it was.
auto_updates_on() {
  local mode="add"
  if [ -f "$UU_CONF" ] && grep -q 'vibesdr-mode: only' "$UU_CONF"; then
    mode="only"
  elif ! [ -f "$UU_CONF" ]; then
    local periodic=""
    eval "$(apt-config shell periodic APT::Periodic::Unattended-Upgrade 2>/dev/null || true)"
    local uu; uu="$(dpkg-query -W -f='${db:Status-Abbrev}' unattended-upgrades 2>/dev/null || true)"
    if [ "${uu#ii}" = "$uu" ] || [ -z "$periodic" ] || [ "$periodic" = 0 ]; then
      mode="only"
    fi
  fi
  if ! command -v unattended-upgrade >/dev/null 2>&1; then
    step "Installing unattended-upgrades (for VibeServer updates only)"
    apt_quiet install --no-install-recommends unattended-upgrades \
      || die 7 "could not install unattended-upgrades."
  fi
  step "Turning on automatic VibeServer updates"
  if [ "$DRY" = 1 ]; then say "   (dry run) would write $UU_CONF (mode: $mode)"; return 0; fi
  {
    echo "// Written by the VibeServer installer ($REPO_URL/install.sh). Delete this file, or run the"
    echo "// installer with --no-auto-updates, to stop automatic VibeServer updates."
    if [ "$mode" = only ]; then
      echo "// vibesdr-mode: only — automatic upgrades were not enabled on this machine, so this file"
      echo "// limits them to the VibeServer repository. It clears the origin lists set in"
      echo "// 50unattended-upgrades: if you later want automatic OS updates too, delete this file first."
      echo '#clear Unattended-Upgrade::Allowed-Origins;'
      echo '#clear Unattended-Upgrade::Origins-Pattern;'
      echo "Unattended-Upgrade::Origins-Pattern { \"$UU_PATTERN\"; };"
      echo 'APT::Periodic::Update-Package-Lists "1";'
      echo 'APT::Periodic::Unattended-Upgrade "1";'
    else
      echo "// vibesdr-mode: add — this machine already ran unattended-upgrades; VibeServer is added to"
      echo "// the origins it was already allowed to upgrade, and nothing else is changed."
      echo "Unattended-Upgrade::Origins-Pattern { \"$UU_PATTERN\"; };"
    fi
  } > "$UU_CONF"
  chmod 0644 "$UU_CONF"
  if [ "$mode" = only ]; then say "   on — VibeServer only; the rest of the system is left as it was"
  else say "   on — added to this machine's existing automatic updates"; fi
}
case "$AUTO" in
  1) auto_updates_on ;;
  0) if [ -f "$UU_CONF" ]; then step "Turning off automatic VibeServer updates"; doit rm -f "$UU_CONF"; fi ;;
esac

# ── Done ───────────────────────────────────────────────────────────────────────────────────────
[ "$DRY" = 1 ] && { say ""; say "Dry run finished; nothing was changed."; return 0; }
[ -n "$AFTER" ] || die 7 "apt reported success but VibeServer is not installed."
IP="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
if [ -z "$IP" ]; then
  IP="$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' || true)"
fi
URL="http://${IP:-<this machine>}:48000"
say ""
if [ -z "$BEFORE" ]; then
  say "VibeServer $AFTER is installed."
  say ""
  say "Next:"
  say "  1. Log out and back in (it added you to the 'plugdev' group, so the radio can be opened)."
  say "  2. Run:  vibeserver      — a short wizard: name, location, radio, admin password."
  say "     It starts the service for you when you finish."
  say "  3. Open: $URL   (on the same network, http://vibeserver.local:48000 usually works too)"
elif [ "$BEFORE" = "$AFTER" ]; then
  say "VibeServer $AFTER is installed and already the latest. Open: $URL"
else
  say "VibeServer upgraded from $BEFORE to $AFTER. Open: $URL"
fi
if [ "$ARCH" = i386 ] || { [ "$ARCH" = armhf ] && ! grep -qw neon /proc/cpuinfo 2>/dev/null; }; then
  say ""
  say "RTL-SDR, Airspy and HackRF need nothing extra. SDRplay RSPs are not supported on this machine."
else
  say ""
  say "RTL-SDR, Airspy, HackRF and SDRplay RSP need nothing extra."
fi
say ""
say "Updates: the admin page's Install updates button, or 'sudo apt update && sudo apt upgrade'."
if [ "$AUTO" != 1 ] && ! [ -f "$UU_CONF" ]; then
  say "To have VibeServer update itself automatically (and nothing else):"
  say "  curl -fsSL $REPO_URL/install.sh | sudo VIBE_AUTO_UPDATES=1 bash"
fi
return 0
}

vibe_install_main "$@"
