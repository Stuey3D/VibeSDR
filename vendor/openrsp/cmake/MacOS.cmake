# macOS build rules.
# Included from cmake/Platform.cmake on APPLE, before any target exists.
#
#   SDRPLAY_MACOS_PLATFORM_SOURCES  src/platform/macos/*.cpp for sdrplay_platform
#   SDRPLAY_MACOS_USB_SOURCES       extra sources for sdrplay_platform_usb (libusb hotplug)
#   sdrplay_macos_client_lib(tgt)   API 3.15 file/install name/exports/rpath for the client dylib
#   sdrplay_macos_finalize()        per-target fix-ups once every target exists
#
# Universal build: pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" (and e.g.
# -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15: clang raises the arm64 slice to 11.0 by itself).
# Targets that link libusb are limited to the architectures the libusb dylib has
# (Homebrew's is thin) unless SDRPLAY_MACOS_LIBUSB_DIR points at a universal build.

set(SDRPLAY_MACOS_NATIVE ON)
set(_mac_src ${CMAKE_CURRENT_LIST_DIR}/../src/platform/macos)
set(SDRPLAY_MACOS_PLATFORM_SOURCES
  ${_mac_src}/SharedMemoryDarwin.cpp
  ${_mac_src}/InterProcessSemaphoreDarwin.cpp
  ${_mac_src}/InterProcessMutexDarwin.cpp
  ${_mac_src}/ThreadOsDarwin.cpp
  ${_mac_src}/ClockDarwin.cpp
  ${_mac_src}/LogSinkDarwin.cpp
  ${_mac_src}/ServiceHostDarwin.cpp)
set(SDRPLAY_MACOS_USB_SOURCES ${_mac_src}/HotplugDarwin.cpp)

# The API 3.15 install root on macOS is /Library/SDRplayAPI/3.15.1; the firmware sets go below it.
set(SDRPLAY_MACOS_ROOT "/Library/SDRplayAPI/3.15.1" CACHE PATH "macOS install root (API 3.15 layout)")
set(SDRPLAY_FIRMWARE_DIR "${SDRPLAY_MACOS_ROOT}/firmware" CACHE PATH
    "Where the service looks for firmware sets (<dir>/own, ...)")

# A private universal libusb instead of pkg-config's.
set(SDRPLAY_MACOS_LIBUSB_DIR "" CACHE PATH "Prefix of a (universal) libusb-1.0 build to use on macOS")
if(SDRPLAY_MACOS_LIBUSB_DIR)
  set(ENV{PKG_CONFIG_PATH} "${SDRPLAY_MACOS_LIBUSB_DIR}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

# 101 exports = cmake/libsdrplay_api.map with the Mach-O leading underscore; the same set in
# both architecture slices.
function(_sdrplay_macos_export_list out)
  file(STRINGS ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/libsdrplay_api.map _lines)
  set(_syms "")
  set(_n 0)
  foreach(_l IN LISTS _lines)
    if(_l MATCHES "^[ \t]+([A-Za-z_][A-Za-z0-9_]*);[ \t]*$")
      string(APPEND _syms "_${CMAKE_MATCH_1}\n")
      math(EXPR _n "${_n} + 1")
    endif()
  endforeach()
  if(NOT _n EQUAL 101)
    message(FATAL_ERROR "libsdrplay_api.map: expected 101 exports, found ${_n}")
  endif()
  file(WRITE ${out}.tmp "${_syms}")
  configure_file(${out}.tmp ${out} COPYONLY) # rewrite only on change
endfunction()

function(sdrplay_macos_client_lib tgt)
  set(_exp ${CMAKE_BINARY_DIR}/libsdrplay_api.exported_symbols)
  _sdrplay_macos_export_list(${_exp})
  # File name libsdrplay_api.so.3.15 (".so" also on macOS), install name
  # @rpath/libsdrplay_api.so.3, no compatibility/current version, rpath /usr/local/lib:
  # the names applications built against API 3.15 look for.
  set_target_properties(${tgt} PROPERTIES
    OUTPUT_NAME sdrplay_api PREFIX "lib" SUFFIX ".so.3.15"
    NO_SONAME ON
    BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "/usr/local/lib"
    LINK_DEPENDS ${_exp})
  target_link_options(${tgt} PRIVATE
    "LINKER:-install_name,@rpath/libsdrplay_api.so.3"
    "LINKER:-exported_symbols_list,${_exp}") # undefined symbols are already an error by default
  # Build-tree name chain like the installed one (.so.3 -> .so.3.15, .dylib/.so -> .so.3)
  # so test programs that recorded @rpath/libsdrplay_api.so.3 find it next to the dylib.
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink libsdrplay_api.so.3.15 $<TARGET_FILE_DIR:${tgt}>/libsdrplay_api.so.3
    COMMAND ${CMAKE_COMMAND} -E create_symlink libsdrplay_api.so.3 $<TARGET_FILE_DIR:${tgt}>/libsdrplay_api.so
    COMMAND ${CMAKE_COMMAND} -E create_symlink libsdrplay_api.so.3 $<TARGET_FILE_DIR:${tgt}>/libsdrplay_api.dylib
    VERBATIM)
endfunction()

# Architectures of the libusb dylib pkg-config found (empty = unknown).
function(_sdrplay_macos_libusb_archs out)
  set(${out} "" PARENT_SCOPE)
  if(NOT LIBUSB_LINK_LIBRARIES)
    return()
  endif()
  list(GET LIBUSB_LINK_LIBRARIES 0 _lib)
  find_program(_LIPO lipo)
  if(NOT _LIPO OR NOT EXISTS "${_lib}")
    return()
  endif()
  execute_process(COMMAND ${_LIPO} -archs ${_lib} OUTPUT_VARIABLE _a OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE _rc ERROR_QUIET)
  if(_rc EQUAL 0)
    string(REPLACE " " ";" _a "${_a}")
    set(${out} "${_a}" PARENT_SCOPE)
  endif()
endfunction()

function(sdrplay_macos_finalize)
  # Service: finds the bundled libusb next to itself (rpath @executable_path/.; the
  # package rewrites the libusb install name).
  if(TARGET sdrplay_apiService)
    set_property(TARGET sdrplay_apiService APPEND PROPERTY INSTALL_RPATH "@executable_path/.")
  endif()
  # Universal request but thin libusb: build the libusb users for the common archs only.
  list(LENGTH CMAKE_OSX_ARCHITECTURES _nreq)
  if(_nreq GREATER 1 AND SDRPLAY_HAVE_LIBUSB)
    _sdrplay_macos_libusb_archs(_have)
    if(_have)
      set(_common "")
      foreach(_a IN LISTS CMAKE_OSX_ARCHITECTURES)
        if(_a IN_LIST _have)
          list(APPEND _common ${_a})
        endif()
      endforeach()
      if(NOT _common STREQUAL CMAKE_OSX_ARCHITECTURES)
        message(WARNING "libusb (${LIBUSB_LINK_LIBRARIES}) has only [${_have}]: the service, rsp_info and "
                        "unit_platform are built for [${_common}] only. Use SDRPLAY_MACOS_LIBUSB_DIR for a "
                        "universal service.")
        foreach(_t sdrplay_platform_usb sdrplay_apiService rsp_info unit_platform)
          if(TARGET ${_t})
            set_target_properties(${_t} PROPERTIES OSX_ARCHITECTURES "${_common}")
          endif()
        endforeach()
      endif()
    endif()
  endif()
  # unit_platform names libsdrplay_platform.a twice on its link line (directly and via
  # sdrplay_platform_usb); Apple's ld (Xcode 15+) warns about that. Harmless: silence it.
  if(TARGET unit_platform AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" SDRPLAY_LD_NO_WARN_DUP_LIBS)
    if(SDRPLAY_LD_NO_WARN_DUP_LIBS)
      target_link_options(unit_platform PRIVATE "LINKER:-no_warn_duplicate_libraries")
    endif()
  endif()
endfunction()
