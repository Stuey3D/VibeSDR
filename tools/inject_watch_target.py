#!/usr/bin/env python3
"""Re-add the VibeSDR Jr watch target to ios/VibeSDR.xcodeproj after `expo prebuild`.

WHY THIS EXISTS
    `expo prebuild` regenerates project.pbxproj from scratch and wipes hand-added targets.
    While the watch target was a thin companion that cost an afternoon in Xcode. It now holds
    the whole Buddy watch app, so the same accident
    would cost a day. This turns it back into one command.

    It is the shipping-project counterpart of spike/WristSDR/tools/genproj.py, which proved the
    technique on the standalone spike. genproj.py's docstring says the spike was kept in its own
    project *because* prebuild wipes such targets. The spike is now the product, so that
    separation has expired — but the risk has not, hence this.

IDEMPOTENT
    Strips every object in the reserved id space before re-inserting, so running it twice is a
    no-op and stale entries from an earlier file list cannot survive.

RESERVED ID SPACE
    AA000002000000000000 + CCxx (target structure), Dxxxx (file refs), Fxxxx (build files).
    ★ EExx in the same prefix belongs to the PHONE-side WCSession module in the main app target
      and is deliberately NOT touched — see reserved_re().
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PBXPROJ = ROOT / "ios" / "VibeSDR.xcodeproj" / "project.pbxproj"

NAME = "VibeSDRWatch"
BUNDLE = "com.vibesdr.app.watchkitapp"  # ★ MUST NOT CHANGE — users have this installed. A new
                                        # id ships a SECOND app instead of an update.
TEAM = "6PV2X6THHM"

# ★ Buddy links NO libopus and runs NO DSP — the phone does both — so the arm64-only constraint
#   that forced the merged app to Series 9+ is GONE. This is pure remote control again, which is
#   what makes the wider hardware reach in the product story true rather than aspirational.
#   The floor is set by the SCREENS, not the radio: Jr's views use handGestureShortcut (the
#   double-tap gesture), which is watchOS 11+. That still reaches Series 6 and later — a real
#   widening from the merged app's 26.0, which meant Series 9+ only.
DEPLOYMENT_TARGET = "11.0"
ARCHS = '"$(ARCHS_STANDARD)"'

# Ids from Expo's own template. Stable across every prebuild so far; asserted, not assumed.
APP_TARGET = "13B07F861A680F5B00A75B9A"
PROJECT_OBJ = "83CBB9F71A601CBA00E9B192"
APP_PRODUCT = "13B07F961A680F5B00A75B9A"  # VibeSDR.app, in the Products group
APP_GROUP = "13B07FAE1A68108700A75B9A"    # the VibeSDR group, in the main group

P = "AA000002000000000000"
TARGET = P + "CC01"
PRODUCT_REF = P + "CC02"
SRC_GROUP = P + "CC03"
PHASE_SRC = P + "CC04"
PHASE_RES = P + "CC05"
PHASE_FRW = P + "CC06"
CFG_LIST = P + "CC07"
CFG_DBG = P + "CC08"
CFG_REL = P + "CC09"
PHASE_EMBED = P + "CC0A"
PROXY = P + "CC0B"
DEPENDENCY = P + "CC0C"
EMBED_BUILD = P + "CC0D"
LOGOS_GROUP = P + "CC10"

# ── Sources: the whole of VibeSDR Jr. Discovered from disk rather than hand-listed, because the
#    merged target IS the app now and a maintained list would drift the first time someone adds
#    a file — the same class of breakage this script exists to prevent.
#
#    WatchLink.swift is DELIBERATELY EXCLUDED. It is the V9 companion's WCSession transport —
#    the one thing the companion contributes — and it becomes `PhoneClient: SDRClient`. It is
#    kept on disk as the reference for that work but does not compile today, because it drove
#    the companion UI, which no longer exists.
NOT_YET_BUILT = set()

WATCH_DIR = ROOT / "ios" / NAME
SOURCES = sorted(p.name for p in WATCH_DIR.glob("*.swift") if p.name not in NOT_YET_BUILT)
LOGOS = sorted(p.name for p in (WATCH_DIR / "Logos").glob("*.png"))
RESOURCES = [("Assets.xcassets", "folder.assetcatalog", None)] + \
            [(n, "image.png", "Logos") for n in LOGOS]
# ★ No opus, no bridging header. Buddy does NO audio and NO DSP — the phone does both — so the
#   whole Opus stack went with the direct clients. Leaving them referenced here would fail the
#   build on files that no longer exist.
PLAIN = [("Info.plist", "text.plist.xml", None)]


def fid(kind, name):
    """Stable id from a name. Regenerating must not churn ids, or every diff is noise."""
    return P + kind + hashlib.sha1(name.encode()).hexdigest()[:4].upper()


def ref_id(name):
    return fid("D", name)


def build_id(name):
    return fid("F", name)


def reserved_re():
    """The id space this script owns. EExx (phone-side module) is excluded on purpose."""
    return re.compile(P + r"(?:CC[0-9A-F]{2}|D[0-9A-F]{4}|F[0-9A-F]{4})\b")


def strip(text):
    """Remove every object definition and reference in the reserved space.

    Brace-counted rather than regexed: objects nest (buildSettings), and a lazy regex would
    stop at the first inner `};` and silently corrupt the project.
    """
    out, lines, i, rx = [], text.split("\n"), 0, reserved_re()
    while i < len(lines):
        line = lines[i]
        if rx.search(line):
            if line.rstrip().endswith("{"):  # a definition opens a block
                depth = 0
                while i < len(lines):
                    depth += lines[i].count("{") - lines[i].count("}")
                    i += 1
                    if depth <= 0:
                        break
                continue
            i += 1  # a bare reference is one line
            continue
        out.append(line)
        i += 1
    return "\n".join(out)


def after(text, anchor, addition):
    eol = text.index("\n", text.index(anchor)) + 1
    return text[:eol] + addition + text[eol:]


def wire(text, obj_id, key, addition):
    """Append a line to the `key = (...)` list inside object `obj_id`."""
    seg = text.index(f"{key} = (", text.index(f"{obj_id} /* "))
    close = text.index("\t\t\t);", seg)
    return text[:close] + addition + "\n" + text[close:]


def generate(text):
    T = "\t\t"
    all_files = [(n, "sourcecode.swift", None) for n in SOURCES] + RESOURCES + PLAIN

    # ── PBXBuildFile
    bf = [f"{T}{EMBED_BUILD} /* {NAME}.app in Embed Watch Content */ = {{isa = PBXBuildFile; "
          f"fileRef = {PRODUCT_REF} /* {NAME}.app */; settings = {{ATTRIBUTES = (RemoveHeadersOnCopy, ); }}; }};",
]
    for n in SOURCES:
        bf.append(f"{T}{build_id(n)} /* {n} in Sources */ = {{isa = PBXBuildFile; "
                  f"fileRef = {ref_id(n)} /* {n} */; }};")
    for n, _, _ in RESOURCES:
        bf.append(f"{T}{build_id(n)} /* {n} in Resources */ = {{isa = PBXBuildFile; "
                  f"fileRef = {ref_id(n)} /* {n} */; }};")
    text = after(text, "/* Begin PBXBuildFile section */", "\n".join(bf) + "\n")

    # ── PBXFileReference
    fr = [f"{T}{PRODUCT_REF} /* {NAME}.app */ = {{isa = PBXFileReference; "
          f"explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; "
          f"sourceTree = BUILT_PRODUCTS_DIR; }};",
]
    for n, kind, _ in all_files:
        fr.append(f"{T}{ref_id(n)} /* {n} */ = {{isa = PBXFileReference; "
                  f"lastKnownFileType = {kind}; path = {n}; sourceTree = \"<group>\"; }};")
    text = after(text, "/* Begin PBXFileReference section */", "\n".join(fr) + "\n")

    # ── Embed Watch Content, on the MAIN app target. Without this the watch app builds but
    #    never ships inside the iOS app, which is how a watch app silently goes missing.
    embed = f"""{T}{PHASE_EMBED} /* Embed Watch Content */ = {{
			isa = PBXCopyFilesBuildPhase;
			buildActionMask = 2147483647;
			dstPath = "$(CONTENTS_FOLDER_PATH)/Watch";
			dstSubfolderSpec = 16;
			files = (
				{EMBED_BUILD} /* {NAME}.app in Embed Watch Content */,
			);
			name = "Embed Watch Content";
			runOnlyForDeploymentPostprocessing = 0;
		}};
"""
    text = section(text, "PBXCopyFilesBuildPhase", embed, "/* End PBXFrameworksBuildPhase section */")

    # ── Groups
    top = [n for n, _, sub in all_files if sub is None]
    children = "\n".join(f"\t\t\t\t{ref_id(n)} /* {n} */," for n in top)
    group = f"""{T}{SRC_GROUP} /* {NAME} */ = {{
			isa = PBXGroup;
			children = (
{children}
				{LOGOS_GROUP} /* Logos */,
			);
			path = {NAME};
			sourceTree = "<group>";
		}};
{T}{LOGOS_GROUP} /* Logos */ = {{
			isa = PBXGroup;
			children = (
{chr(10).join(f"				{ref_id(n)} /* {n} */," for n in LOGOS)}
			);
			path = Logos;
			sourceTree = "<group>";
		}};
"""
    text = after(text, "/* Begin PBXGroup section */", group)

    # ── Native target
    text = after(text, "/* Begin PBXNativeTarget section */", f"""{T}{TARGET} /* {NAME} */ = {{
			isa = PBXNativeTarget;
			buildConfigurationList = {CFG_LIST} /* Build configuration list for PBXNativeTarget "{NAME}" */;
			buildPhases = (
				{PHASE_SRC} /* Sources */,
				{PHASE_FRW} /* Frameworks */,
				{PHASE_RES} /* Resources */,
			);
			buildRules = (
			);
			dependencies = (
			);
			name = {NAME};
			productName = {NAME};
			productReference = {PRODUCT_REF} /* {NAME}.app */;
			productType = "com.apple.product-type.application";
		}};
""")

    # ── Dependency, so building the app builds the watch app first
    text = section(text, "PBXContainerItemProxy", f"""{T}{PROXY} /* PBXContainerItemProxy */ = {{
			isa = PBXContainerItemProxy;
			containerPortal = {PROJECT_OBJ} /* Project object */;
			proxyType = 1;
			remoteGlobalIDString = {TARGET};
			remoteInfo = {NAME};
		}};
""", "/* End PBXNativeTarget section */")
    text = section(text, "PBXTargetDependency", f"""{T}{DEPENDENCY} /* PBXTargetDependency */ = {{
			isa = PBXTargetDependency;
			target = {TARGET} /* {NAME} */;
			targetProxy = {PROXY} /* PBXContainerItemProxy */;
		}};
""", "/* End PBXContainerItemProxy section */")

    # ── Build phases
    src_files = "\n".join(f"\t\t\t\t{build_id(n)} /* {n} in Sources */," for n in SOURCES)
    res_files = "\n".join(f"\t\t\t\t{build_id(n)} /* {n} in Resources */," for n, _, _ in RESOURCES)
    text = after(text, "/* Begin PBXSourcesBuildPhase section */", f"""{T}{PHASE_SRC} /* Sources */ = {{
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{src_files}
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
""")
    text = after(text, "/* Begin PBXResourcesBuildPhase section */", f"""{T}{PHASE_RES} /* Resources */ = {{
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{res_files}
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
""")
    text = after(text, "/* Begin PBXFrameworksBuildPhase section */", f"""{T}{PHASE_FRW} /* Frameworks */ = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
""")

    # ── Build configurations. Versions are READ FROM the main app target rather than hardcoded,
    #    so a version bump can never leave the watch behind — a mismatch is rejected at submission.
    version = re.search(r"CURRENT_PROJECT_VERSION = (\d+);", text).group(1)
    marketing = re.search(r"MARKETING_VERSION = ([\d.]+);", text).group(1)

    def cfg(cid, name, extra):
        return f"""{T}{cid} /* {name} */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ARCHS = {ARCHS};
				ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;
				CLANG_ENABLE_MODULES = YES;
				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = {version};
				DEVELOPMENT_TEAM = {TEAM};
				GENERATE_INFOPLIST_FILE = NO;
				INFOPLIST_FILE = {NAME}/Info.plist;
				LD_RUNPATH_SEARCH_PATHS = (
					"$(inherited)",
					"@executable_path/Frameworks",
				);
				LIBRARY_SEARCH_PATHS = "$(inherited)";
				MARKETING_VERSION = {marketing};
				OTHER_CFLAGS = "$(inherited)";
				OTHER_CPLUSPLUSFLAGS = "$(inherited)";
				PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE};
				PRODUCT_NAME = "$(TARGET_NAME)";
				SDKROOT = watchos;
				SKIP_INSTALL = YES;
				SUPPORTED_PLATFORMS = "watchos watchsimulator";
{extra}				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 4;
				WATCHOS_DEPLOYMENT_TARGET = {DEPLOYMENT_TARGET};
			}};
			name = {name};
		}};
"""
    text = after(text, "/* Begin XCBuildConfiguration section */",
                 cfg(CFG_DBG, "Debug", '\t\t\t\tSWIFT_OPTIMIZATION_LEVEL = "-Onone";\n')
                 + cfg(CFG_REL, "Release", '\t\t\t\tSWIFT_COMPILATION_MODE = wholemodule;\n'))
    text = after(text, "/* Begin XCConfigurationList section */",
                 f"""{T}{CFG_LIST} /* Build configuration list for PBXNativeTarget "{NAME}" */ = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{CFG_DBG} /* Debug */,
				{CFG_REL} /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
""")

    # ── Wire into the main target and the project
    text = wire(text, APP_TARGET, "buildPhases", f"\t\t\t\t{PHASE_EMBED} /* Embed Watch Content */,")
    text = wire(text, APP_TARGET, "dependencies", f"\t\t\t\t{DEPENDENCY} /* PBXTargetDependency */,")
    # Xcode writes TargetAttributes itself on first open; without it the next person gets a
    # spurious diff rather than a build failure.
    text = insert_after_line(text, "TargetAttributes = {",
                             f"\t\t\t\t\t{TARGET} = {{\n\t\t\t\t\t\tCreatedOnToolsVersion = 26.0;\n\t\t\t\t\t}};")
    text = insert_after_line(text, f"{APP_PRODUCT} /* VibeSDR.app */,",
                             f"\t\t\t\t{PRODUCT_REF} /* {NAME}.app */,")
    text = insert_after_line(text, f"{APP_GROUP} /* VibeSDR */,",
                             f"\t\t\t\t{SRC_GROUP} /* {NAME} */,")
    text = insert_after_line(text, f"{APP_TARGET} /* VibeSDR */,",
                             f"\t\t\t\t{TARGET} /* {NAME} */,")
    return text


def section(text, kind, body, fallback_anchor):
    """Insert into an existing pbxproj section, creating it if prebuild left it out."""
    begin = f"/* Begin {kind} section */"
    if begin in text:
        return after(text, begin, body)
    return after(text, fallback_anchor,
                 f"\n{begin}\n{body}/* End {kind} section */\n")


def insert_after_line(text, anchor_line, addition):
    eol = text.index("\n", text.index(anchor_line)) + 1
    return text[:eol] + addition + "\n" + text[eol:]


def main():
    text = PBXPROJ.read_text()
    for anchor, what in [(APP_TARGET, "main app target"), (PROJECT_OBJ, "project object")]:
        if anchor not in text:
            sys.exit(f"error: {what} id {anchor} not found — Expo's template ids have moved, "
                     f"update the constants at the top of this script")
    ids = [ref_id(n) for n, _, _ in
           [(s, None, None) for s in SOURCES] + RESOURCES + PLAIN]
    if len(set(ids)) != len(ids):
        sys.exit("error: hashed id collision — widen the hash slice in fid()")

    PBXPROJ.write_text(generate(strip(text)))
    print(f"injected {NAME}: {len(SOURCES)} sources, {len(RESOURCES)} resources")
    if NOT_YET_BUILT:
        print(f"  not built (by design): {', '.join(sorted(NOT_YET_BUILT))}")

    r = subprocess.run(["xcodebuild", "-list", "-project", str(PBXPROJ.parent)],
                       capture_output=True, text=True)
    if r.returncode != 0 or NAME not in r.stdout:
        sys.exit(f"error: xcodebuild -list rejected the result\n{r.stdout}{r.stderr}")
    print("verified: xcodebuild -list parses the project and sees both targets")


if __name__ == "__main__":
    main()
