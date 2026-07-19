#!/usr/bin/env python3
"""Re-add the VibeSDR Jr watch target to ios/VibeSDR.xcodeproj after `expo prebuild`.

WHY THIS EXISTS
    `expo prebuild` regenerates project.pbxproj from scratch and wipes hand-added targets.
    Today that costs an afternoon of clicking in Xcode. After the watch-app merge that target
    holds the ENTIRE Jr codebase (~32 Swift files + libopus) instead of a thin companion, so
    the same accident becomes a day's work. This turns it back into `python3 tools/inject_watch_target.py`.

    It is the shipping-project counterpart of spike/WristSDR/tools/genproj.py, which proved the
    technique on the standalone spike. genproj.py's docstring says the spike was kept in its own
    project *because* prebuild wipes such targets. The spike is now the product, so that
    separation has expired — but the risk has not, hence this.

IDEMPOTENT
    Strips every object owning a reserved id before re-inserting, so running it twice is a no-op
    and running it on an already-good project is safe.

SCOPE
    The watch TARGET only (ids AA000002…CC*/DA*). The phone-side WCSession native module
    (AA000002…EE*, in the main app target) and the other hand-added iOS sources (AA000001…)
    are also prebuild-fragile and want the same treatment — deliberately not done here, so this
    can be proven against the shipping project one target at a time.
"""
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
DEPLOYMENT_TARGET = "10.0"

# The main app target and project object we splice into. These ids come from Expo's own
# template and have been stable across every prebuild so far; assert rather than assume.
APP_TARGET = "13B07F861A680F5B00A75B9A"
PROJECT_OBJ = "83CBB9F71A601CBA00E9B192"
APP_PRODUCT = "13B07F961A680F5B00A75B9A"  # VibeSDR.app, in the Products group
APP_GROUP = "13B07FAE1A68108700A75B9A"    # the VibeSDR group, in the main group

# ── Reserved ids. Hand-assigned rather than hashed so a regenerated project diffs cleanly
#    against the hand-built one it replaces, and so the prefix stays greppable.
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

# Sources, in build order. `path` is relative to the VibeSDRWatch group.
#   ★ After the merge this list becomes the spike's 32 files — that is the whole point of
#     driving the target from a list rather than from Xcode's UI.
SOURCES = [
    ("VibeSDRWatchApp.swift", "DA01"),
    ("ContentView.swift", "DA04"),
    ("NumpadView.swift", "DA07"),
    ("FmdxView.swift", "DA09"),
    ("DabView.swift", "DA0A"),
    ("AircraftView.swift", "DA0B"),
    ("ControlMenu.swift", "DA08"),
    ("WatchLink.swift", "DA02"),
    ("CpuMeter.swift", "DA0C"),
    ("WaterfallBuffer.swift", "DA03"),
]
RESOURCES = [("Assets.xcassets", "DA05", "folder.assetcatalog")]
PLAIN = [("Info.plist", "DA06", "text.plist.xml")]  # in the group, not in a build phase


def build_file_id(ref):
    """DA0x file ref → DA1x build-file id. Keeps the two visibly paired when grepping."""
    return P + "DA1" + ref[3]


def reserved_ids():
    ids = {TARGET, PRODUCT_REF, SRC_GROUP, PHASE_SRC, PHASE_RES, PHASE_FRW, CFG_LIST,
           CFG_DBG, CFG_REL, PHASE_EMBED, PROXY, DEPENDENCY, EMBED_BUILD}
    for entry in SOURCES + RESOURCES + PLAIN:
        ref = entry[1]
        ids.add(P + ref)
        ids.add(build_file_id(ref))
    return ids


def strip(text, ids):
    """Remove every object definition and every reference to `ids`.

    Brace-counted rather than regexed, because multi-line objects nest (buildSettings) and a
    lazy regex would stop at the first inner `};` and silently corrupt the project.
    """
    out, lines, i = [], text.split("\n"), 0
    id_re = re.compile("|".join(sorted(ids)))
    while i < len(lines):
        line = lines[i]
        if id_re.search(line):
            # A definition opens a block; a reference is a single line we simply drop.
            if line.rstrip().endswith("{"):
                depth = 0
                while i < len(lines):
                    depth += lines[i].count("{") - lines[i].count("}")
                    i += 1
                    if depth <= 0:
                        break
                continue
            i += 1
            continue
        out.append(line)
        i += 1
    return "\n".join(out)


def after(text, anchor, addition):
    """Insert `addition` immediately after the line containing `anchor`."""
    idx = text.index(anchor)
    eol = text.index("\n", idx) + 1
    return text[:eol] + addition + text[eol:]


def generate(text):
    T = "\t\t"

    # ── PBXBuildFile
    bf = [f"{T}{EMBED_BUILD} /* {NAME}.app in Embed Watch Content */ = {{isa = PBXBuildFile; "
          f"fileRef = {PRODUCT_REF} /* {NAME}.app */; settings = {{ATTRIBUTES = (RemoveHeadersOnCopy, ); }}; }};"]
    for name, ref in SOURCES:
        bf.append(f"{T}{build_file_id(ref)} /* {name} in Sources */ = {{isa = PBXBuildFile; "
                  f"fileRef = {P}{ref} /* {name} */; }};")
    for name, ref, _ in RESOURCES:
        bf.append(f"{T}{build_file_id(ref)} /* {name} in Resources */ = {{isa = PBXBuildFile; "
                  f"fileRef = {P}{ref} /* {name} */; }};")
    text = after(text, "/* Begin PBXBuildFile section */", "\n".join(bf) + "\n")

    # ── PBXFileReference
    fr = [f"{T}{PRODUCT_REF} /* {NAME}.app */ = {{isa = PBXFileReference; "
          f"explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; "
          f"sourceTree = BUILT_PRODUCTS_DIR; }};"]
    for name, ref in SOURCES:
        fr.append(f"{T}{P}{ref} /* {name} */ = {{isa = PBXFileReference; "
                  f"lastKnownFileType = sourcecode.swift; path = {name}; sourceTree = \"<group>\"; }};")
    for name, ref, kind in RESOURCES + PLAIN:
        fr.append(f"{T}{P}{ref} /* {name} */ = {{isa = PBXFileReference; "
                  f"lastKnownFileType = {kind}; path = {name}; sourceTree = \"<group>\"; }};")
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
    if "/* Begin PBXCopyFilesBuildPhase section */" in text:
        text = after(text, "/* Begin PBXCopyFilesBuildPhase section */", embed)
    else:
        text = after(text, "/* End PBXFrameworksBuildPhase section */",
                     "\n/* Begin PBXCopyFilesBuildPhase section */\n" + embed
                     + "/* End PBXCopyFilesBuildPhase section */\n")

    # ── Group
    children = "\n".join(f"\t\t\t\t{P}{ref} /* {name} */,"
                         for name, ref in SOURCES) + "\n" + "\n".join(
        f"\t\t\t\t{P}{ref} /* {name} */," for name, ref, _ in RESOURCES + PLAIN)
    group = f"""{T}{SRC_GROUP} /* {NAME} */ = {{
			isa = PBXGroup;
			children = (
{children}
			);
			path = {NAME};
			sourceTree = "<group>";
		}};
"""
    text = after(text, "/* Begin PBXGroup section */", group)

    # ── Native target
    target = f"""{T}{TARGET} /* {NAME} */ = {{
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
"""
    text = after(text, "/* Begin PBXNativeTarget section */", target)

    # ── Dependency, so building the app builds the watch app first
    proxy = f"""{T}{PROXY} /* PBXContainerItemProxy */ = {{
			isa = PBXContainerItemProxy;
			containerPortal = {PROJECT_OBJ} /* Project object */;
			proxyType = 1;
			remoteGlobalIDString = {TARGET};
			remoteInfo = {NAME};
		}};
"""
    dep = f"""{T}{DEPENDENCY} /* PBXTargetDependency */ = {{
			isa = PBXTargetDependency;
			target = {TARGET} /* {NAME} */;
			targetProxy = {PROXY} /* PBXContainerItemProxy */;
		}};
"""
    if "/* Begin PBXContainerItemProxy section */" in text:
        text = after(text, "/* Begin PBXContainerItemProxy section */", proxy)
    else:
        text = after(text, "/* End PBXNativeTarget section */",
                     "\n/* Begin PBXContainerItemProxy section */\n" + proxy
                     + "/* End PBXContainerItemProxy section */\n")
    if "/* Begin PBXTargetDependency section */" in text:
        text = after(text, "/* Begin PBXTargetDependency section */", dep)
    else:
        text = after(text, "/* End PBXContainerItemProxy section */",
                     "\n/* Begin PBXTargetDependency section */\n" + dep
                     + "/* End PBXTargetDependency section */\n")

    # ── Build phases
    src_files = "\n".join(f"\t\t\t\t{build_file_id(ref)} /* {name} in Sources */,"
                          for name, ref in SOURCES)
    res_files = "\n".join(f"\t\t\t\t{build_file_id(ref)} /* {name} in Resources */,"
                          for name, ref, _ in RESOURCES)
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

    # ── Build configurations. CURRENT_PROJECT_VERSION and MARKETING_VERSION are read from the
    #    main app target rather than hardcoded, so a version bump can never leave the watch
    #    behind — a mismatch there is rejected at submission.
    version = re.search(r"CURRENT_PROJECT_VERSION = (\d+);", text).group(1)
    marketing = re.search(r"MARKETING_VERSION = ([\d.]+);", text).group(1)

    def cfg(cid, name, extra):
        return f"""{T}{cid} /* {name} */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
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
                 + cfg(CFG_REL, "Release", ""))
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

    # ── Wire into the main target: embed phase, dependency, products group, target list.
    text = wire(text, APP_TARGET, "buildPhases", f"\t\t\t\t{PHASE_EMBED} /* Embed Watch Content */,")
    text = wire(text, APP_TARGET, "dependencies", f"\t\t\t\t{DEPENDENCY} /* PBXTargetDependency */,")
    # TargetAttributes. Xcode adds this itself the moment you open the project, so a missing
    # entry produces a spurious diff on the next person's machine rather than a build failure.
    text = insert_after_line(text, "TargetAttributes = {", f"""\t\t\t\t\t{TARGET} = {{
						CreatedOnToolsVersion = 26.0;
					}};""")

    text = insert_after_line(text, f"{APP_PRODUCT} /* VibeSDR.app */,",
                             f"\t\t\t\t{PRODUCT_REF} /* {NAME}.app */,")
    text = insert_after_line(text, f"{APP_GROUP} /* VibeSDR */,",
                             f"\t\t\t\t{SRC_GROUP} /* {NAME} */,")
    text = insert_after_line(text, f"{APP_TARGET} /* VibeSDR */,",
                             f"\t\t\t\t{TARGET} /* {NAME} */,")
    return text


def insert_after_line(text, anchor_line, addition):
    idx = text.index(anchor_line)
    eol = text.index("\n", idx) + 1
    return text[:eol] + addition + "\n" + text[eol:]


def wire(text, obj_id, key, addition):
    """Append a line to the `key = (...)` list inside object `obj_id`."""
    start = text.index(f"{obj_id} /* ")
    seg = text.index(f"{key} = (", start)
    close = text.index("\t\t\t);", seg)
    return text[:close] + addition + "\n" + text[close:]


def main():
    text = PBXPROJ.read_text()
    for anchor, what in [(APP_TARGET, "main app target"), (PROJECT_OBJ, "project object")]:
        if anchor not in text:
            sys.exit(f"error: {what} id {anchor} not found — Expo's template ids have moved, "
                     f"update the constants at the top of this script")
    out = generate(strip(text, reserved_ids()))
    PBXPROJ.write_text(out)
    print(f"injected {NAME} target ({len(SOURCES)} sources) into {PBXPROJ.relative_to(ROOT)}")
    # Proof, not faith: the project must still parse and still list both targets.
    r = subprocess.run(["xcodebuild", "-list", "-project", str(PBXPROJ.parent)],
                       capture_output=True, text=True)
    if r.returncode != 0 or NAME not in r.stdout:
        sys.exit(f"error: xcodebuild -list rejected the result\n{r.stdout}{r.stderr}")
    print("verified: xcodebuild -list parses the project and sees both targets")


if __name__ == "__main__":
    main()
