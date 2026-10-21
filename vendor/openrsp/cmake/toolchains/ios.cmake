# iOS / iPadOS (device or simulator) build of the portable core and its unit tests, on a Mac:
#   cmake -S . -B build-iossim -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake \
#         [-DSDRPLAY_IOS_SDK=iphoneos]   (default: iphonesimulator)
# Plain Mach-O executables (no app bundles), so the simulator can run the tests with
# `xcrun simctl spawn <device> <exe>`. No signing. There is no USB here (an iPad needs a
# USBDriverKit extension), and the host's Homebrew pkg-config files must not leak in.
set(CMAKE_SYSTEM_NAME iOS)
set(SDRPLAY_IOS_SDK "iphonesimulator" CACHE STRING "iphonesimulator | iphoneos")
set(CMAKE_OSX_SYSROOT ${SDRPLAY_IOS_SDK})
if(NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES arm64)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
endif()
set(CMAKE_MACOSX_BUNDLE OFF)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent")
set(ENV{PKG_CONFIG_PATH} "")
