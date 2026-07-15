#!/usr/bin/env python3
"""Generate WristSDR.xcodeproj by hand.

A SEPARATE project, deliberately. Adding a target to VibeSDR.xcodeproj would put the spike
inside the same pbxproj as the hand-added watch and native-module targets — and `expo
prebuild` is documented to wipe exactly those. A throwaway measurement rig must not be able
to damage the shipping app, so it lives somewhere it cannot reach.
"""
import os, hashlib

ROOT = "/Users/stuey3d/VibeSDR/spike/WristSDR"
NAME = "WristSDR"
BUNDLE = "com.stuey3d.wristsdr"
TEAM = "6PV2X6THHM"

SOURCES = [
    "WristSDRApp.swift", "ContentView.swift", "UberClient.swift",
    "SignalProcessor.swift", "WaterfallBuffer.swift", "OpusDecoder.swift",
    "WatchAudio.swift", "AudioSocket.swift", "Vitals.swift", "Viridis.swift", "Gzip.swift",
]

def uid(s):
    """Stable 24-hex ids — a regenerated project should be byte-identical, not churn."""
    return hashlib.sha1(s.encode()).hexdigest()[:24].upper()

# ── ids
PROJECT      = uid("project")
MAIN_GROUP   = uid("maingroup")
SRC_GROUP    = uid("srcgroup")
PRODUCTS     = uid("products")
TARGET       = uid("target")
PRODUCT_REF  = uid("productref")
CFG_LIST_P   = uid("cfglistproj")
CFG_LIST_T   = uid("cfglisttgt")
CFG_P_DBG    = uid("cfgpdbg")
CFG_P_REL    = uid("cfgprel")
CFG_T_DBG    = uid("cfgtdbg")
CFG_T_REL    = uid("cfgtrel")
PHASE_SRC    = uid("phasesrc")
PHASE_FRW    = uid("phasefrw")
PHASE_RES    = uid("phaseres")
PLIST_REF    = uid("plistref")
BRIDGE_REF   = uid("bridgeref")
OPUS_GROUP   = uid("opusgroup")
OPUS_LIB_REF = uid("opuslibref")
OPUS_LIB_BLD = uid("opuslibbld")

file_refs, build_files, src_children = [], [], []
for s in SOURCES:
    fr, bf = uid("fr:" + s), uid("bf:" + s)
    file_refs.append(f'\t\t{fr} /* {s} */ = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = "{s}"; sourceTree = "<group>"; }};')
    build_files.append(f'\t\t{bf} /* {s} in Sources */ = {{isa = PBXBuildFile; fileRef = {fr} /* {s} */; }};')
    src_children.append(f'\t\t\t\t{fr} /* {s} */,')

src_phase_files = "\n".join(
    f'\t\t\t\t{uid("bf:" + s)} /* {s} in Sources */,' for s in SOURCES)

pbx = f'''// !$*UTF8*$!
{{
	archiveVersion = 1;
	classes = {{}};
	objectVersion = 56;
	objects = {{

/* Begin PBXBuildFile section */
{chr(10).join(build_files)}
		{OPUS_LIB_BLD} /* libopus.a in Frameworks */ = {{isa = PBXBuildFile; fileRef = {OPUS_LIB_REF} /* libopus.a */; }};
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
{chr(10).join(file_refs)}
		{PLIST_REF} /* Info.plist */ = {{isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = Info.plist; sourceTree = "<group>"; }};
		{BRIDGE_REF} /* {NAME}-Bridging-Header.h */ = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "{NAME}-Bridging-Header.h"; sourceTree = "<group>"; }};
		{OPUS_LIB_REF} /* libopus.a */ = {{isa = PBXFileReference; lastKnownFileType = archive.ar; path = libopus.a; sourceTree = "<group>"; }};
		{PRODUCT_REF} /* {NAME}.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = "{NAME}.app"; sourceTree = BUILT_PRODUCTS_DIR; }};
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		{PHASE_FRW} /* Frameworks */ = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{OPUS_LIB_BLD} /* libopus.a in Frameworks */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXGroup section */
		{MAIN_GROUP} = {{
			isa = PBXGroup;
			children = (
				{SRC_GROUP} /* {NAME} */,
				{PRODUCTS} /* Products */,
			);
			sourceTree = "<group>";
		}};
		{SRC_GROUP} /* {NAME} */ = {{
			isa = PBXGroup;
			children = (
{chr(10).join(src_children)}
				{BRIDGE_REF} /* {NAME}-Bridging-Header.h */,
				{PLIST_REF} /* Info.plist */,
				{OPUS_GROUP} /* opus */,
			);
			path = "{NAME}";
			sourceTree = "<group>";
		}};
		{OPUS_GROUP} /* opus */ = {{
			isa = PBXGroup;
			children = (
				{OPUS_LIB_REF} /* libopus.a */,
			);
			path = opus;
			sourceTree = "<group>";
		}};
		{PRODUCTS} /* Products */ = {{
			isa = PBXGroup;
			children = (
				{PRODUCT_REF} /* {NAME}.app */,
			);
			name = Products;
			sourceTree = "<group>";
		}};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		{TARGET} /* {NAME} */ = {{
			isa = PBXNativeTarget;
			buildConfigurationList = {CFG_LIST_T};
			buildPhases = (
				{PHASE_SRC} /* Sources */,
				{PHASE_FRW} /* Frameworks */,
				{PHASE_RES} /* Resources */,
			);
			buildRules = ();
			dependencies = ();
			name = "{NAME}";
			productName = "{NAME}";
			productReference = {PRODUCT_REF} /* {NAME}.app */;
			productType = "com.apple.product-type.application";
		}};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		{PROJECT} /* Project object */ = {{
			isa = PBXProject;
			attributes = {{
				BuildIndependentTargetsInParallel = 1;
				LastSwiftUpdateCheck = 2700;
				LastUpgradeCheck = 2700;
				TargetAttributes = {{
					{TARGET} = {{
						CreatedOnToolsVersion = 27.0;
					}};
				}};
			}};
			buildConfigurationList = {CFG_LIST_P};
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (en, Base);
			mainGroup = {MAIN_GROUP};
			productRefGroup = {PRODUCTS} /* Products */;
			projectDirPath = "";
			projectRoot = "";
			targets = (
				{TARGET} /* {NAME} */,
			);
		}};
/* End PBXProject section */

/* Begin PBXResourcesBuildPhase section */
		{PHASE_RES} /* Resources */ = {{
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = ();
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXResourcesBuildPhase section */

/* Begin PBXSourcesBuildPhase section */
		{PHASE_SRC} /* Sources */ = {{
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{src_phase_files}
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		{CFG_P_DBG} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ALWAYS_SEARCH_USER_PATHS = NO;
				CLANG_ENABLE_MODULES = YES;
				CLANG_ENABLE_OBJC_ARC = YES;
				COPY_PHASE_STRIP = NO;
				DEBUG_INFORMATION_FORMAT = dwarf;
				ENABLE_STRICT_OBJC_MSGSEND = YES;
				GCC_OPTIMIZATION_LEVEL = 0;
				GCC_PREPROCESSOR_DEFINITIONS = ("DEBUG=1", "$(inherited)");
				SDKROOT = watchos;
				/* arm64 ONLY. watchOS defaults to building arm64_32 as well (Series 4-8),
				   and our libopus has no such slice — nor should it: JR targets Series 9+,
				   which is exactly where Apple went 64-bit and where they are dropping the
				   Ultra 1 in watchOS 27. The link error IS the product decision. */
				ARCHS = arm64;
				SWIFT_ACTIVE_COMPILATION_CONDITIONS = DEBUG;
				SWIFT_OPTIMIZATION_LEVEL = "-Onone";
				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 4;
				WATCHOS_DEPLOYMENT_TARGET = 26.0;
				ONLY_ACTIVE_ARCH = YES;
			}};
			name = Debug;
		}};
		{CFG_P_REL} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ALWAYS_SEARCH_USER_PATHS = NO;
				CLANG_ENABLE_MODULES = YES;
				CLANG_ENABLE_OBJC_ARC = YES;
				COPY_PHASE_STRIP = NO;
				DEBUG_INFORMATION_FORMAT = "dwarf-with-dsym";
				ENABLE_NS_ASSERTIONS = NO;
				ENABLE_STRICT_OBJC_MSGSEND = YES;
				SDKROOT = watchos;
				ARCHS = arm64;
				SWIFT_COMPILATION_MODE = wholemodule;
				SWIFT_OPTIMIZATION_LEVEL = "-O";
				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 4;
				WATCHOS_DEPLOYMENT_TARGET = 26.0;
				VALIDATE_PRODUCT = YES;
			}};
			name = Release;
		}};
		{CFG_T_DBG} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;
				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = {TEAM};
				ENABLE_PREVIEWS = YES;
				GENERATE_INFOPLIST_FILE = NO;
				INFOPLIST_FILE = "{NAME}/Info.plist";
				INFOPLIST_KEY_CFBundleDisplayName = "{NAME}";
				LD_RUNPATH_SEARCH_PATHS = ("$(inherited)", "@executable_path/Frameworks");
				MARKETING_VERSION = 1.0;
				PRODUCT_BUNDLE_IDENTIFIER = "{BUNDLE}";
				PRODUCT_NAME = "$(TARGET_NAME)";
				SKIP_INSTALL = NO;
				SWIFT_EMIT_LOC_STRINGS = YES;
				SWIFT_OBJC_BRIDGING_HEADER = "{NAME}/{NAME}-Bridging-Header.h";
				HEADER_SEARCH_PATHS = ("$(inherited)", "$(SRCROOT)/{NAME}/opus/include/opus");
				LIBRARY_SEARCH_PATHS = ("$(inherited)", "$(SRCROOT)/{NAME}/opus");
				WATCHOS_DEPLOYMENT_TARGET = 26.0;
			}};
			name = Debug;
		}};
		{CFG_T_REL} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;
				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = {TEAM};
				ENABLE_PREVIEWS = YES;
				GENERATE_INFOPLIST_FILE = NO;
				INFOPLIST_FILE = "{NAME}/Info.plist";
				INFOPLIST_KEY_CFBundleDisplayName = "{NAME}";
				LD_RUNPATH_SEARCH_PATHS = ("$(inherited)", "@executable_path/Frameworks");
				MARKETING_VERSION = 1.0;
				PRODUCT_BUNDLE_IDENTIFIER = "{BUNDLE}";
				PRODUCT_NAME = "$(TARGET_NAME)";
				SKIP_INSTALL = NO;
				SWIFT_EMIT_LOC_STRINGS = YES;
				SWIFT_OBJC_BRIDGING_HEADER = "{NAME}/{NAME}-Bridging-Header.h";
				HEADER_SEARCH_PATHS = ("$(inherited)", "$(SRCROOT)/{NAME}/opus/include/opus");
				LIBRARY_SEARCH_PATHS = ("$(inherited)", "$(SRCROOT)/{NAME}/opus");
				WATCHOS_DEPLOYMENT_TARGET = 26.0;
			}};
			name = Release;
		}};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		{CFG_LIST_P} = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{CFG_P_DBG} /* Debug */,
				{CFG_P_REL} /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
		{CFG_LIST_T} = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{CFG_T_DBG} /* Debug */,
				{CFG_T_REL} /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
/* End XCConfigurationList section */
	}};
	rootObject = {PROJECT} /* Project object */;
}}
'''

projdir = os.path.join(ROOT, f"{NAME}.xcodeproj")
os.makedirs(projdir, exist_ok=True)
with open(os.path.join(projdir, "project.pbxproj"), "w") as f:
    f.write(pbx)

# A shared scheme, so xcodebuild -scheme works without opening Xcode at all.
schemedir = os.path.join(projdir, "xcshareddata", "xcschemes")
os.makedirs(schemedir, exist_ok=True)
scheme = f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "2700" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "YES" buildForRunning = "YES" buildForProfiling = "YES" buildForArchiving = "YES" buildForAnalyzing = "YES">
            <BuildableReference
               BuildableIdentifier = "primary"
               BlueprintIdentifier = "{TARGET}"
               BuildableName = "{NAME}.app"
               BlueprintName = "{NAME}"
               ReferencedContainer = "container:{NAME}.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <LaunchAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" debugServiceExtension = "internal" allowLocationSimulation = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "{TARGET}"
            BuildableName = "{NAME}.app"
            BlueprintName = "{NAME}"
            ReferencedContainer = "container:{NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction buildConfiguration = "Release" shouldUseLaunchSchemeArgsEnv = "YES" savedToolIdentifier = "" useCustomWorkingDirectory = "NO" debugDocumentVersioning = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "{TARGET}"
            BuildableName = "{NAME}.app"
            BlueprintName = "{NAME}"
            ReferencedContainer = "container:{NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </ProfileAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
'''
with open(os.path.join(schemedir, f"{NAME}.xcscheme"), "w") as f:
    f.write(scheme)

print("wrote", projdir)
