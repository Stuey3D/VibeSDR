# check-deb-depends.cmake — CPACK_POST_BUILD_SCRIPTS: refuse a package that depends on a library.
#
# ★★★ THE RULE (Stuart, 2026-10-18): the VibeServer .deb needs NOTHING from the system for SDR
#     support or libraries except glibc. FFmpeg is a Recommends (DAB+, exec'd) and the SDRplay API
#     is the owner's own install (dlopen'd), so neither can appear here. Depends may name libc6 and
#     the tools we exec (CPACK_VIBE_ALLOWED_DEPENDS, set in CMakeLists.txt) — nothing else.
# ★★ WHY A GUARD AND NOT A HABIT. dpkg-shlibdeps adds a Depends line for every shared library the
#    ELF names, silently. One -dev package left in a build image, one find_library that settles for a
#    .so, one dynamic libatomic, and the package refuses the next distribution — exactly how
#    libhackrf0 (>= 2022.09.1) kept 11.0.x off Ubuntu 22.04. So the PACKAGE is checked, not the build:
#    the offending .deb is deleted and cpack fails, and nothing can publish it by accident.
foreach(_deb IN LISTS CPACK_PACKAGE_FILES)
  if(NOT _deb MATCHES "\\.deb$")
    continue()
  endif()
  execute_process(COMMAND dpkg-deb -f "${_deb}" Depends
                  OUTPUT_VARIABLE _dep OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    file(REMOVE "${_deb}")
    message(FATAL_ERROR "check-deb-depends: cannot read Depends from ${_deb} — package removed")
  endif()
  string(REPLACE "," ";" _items "${_dep}")
  set(_bad "")
  foreach(_it IN LISTS _items)
    string(STRIP "${_it}" _it)
    # "libc6 (>= 2.34)" → libc6 ; an alternative ("a | b") is refused outright — we never write one.
    string(REGEX REPLACE " *\\(.*\\)$" "" _name "${_it}")
    if(_name STREQUAL "")
      continue()
    endif()
    list(FIND CPACK_VIBE_ALLOWED_DEPENDS "${_name}" _i)
    if(_i EQUAL -1)
      list(APPEND _bad "${_it}")
    elseif(_name STREQUAL "libc6" AND CPACK_VIBE_MAX_LIBC)
      # ★★ The floor too: one new glibc symbol (arc4random, 2.36) refused Ubuntu 22.04 on its own.
      if(_it MATCHES "\\(>= ([0-9.]+)\\)")
        set(_floor "${CMAKE_MATCH_1}")
        if(_floor VERSION_GREATER CPACK_VIBE_MAX_LIBC)
          list(APPEND _bad "${_it} (above glibc ${CPACK_VIBE_MAX_LIBC} — see linux/glibc_floor.c)")
        endif()
      endif()
    endif()
  endforeach()
  if(_bad)
    file(REMOVE "${_deb}")
    string(REPLACE ";" ", " _bad "${_bad}")
    message(FATAL_ERROR
            "check-deb-depends: ${_deb} depends on: ${_bad}\n"
            "  Only ${CPACK_VIBE_ALLOWED_DEPENDS} are allowed (libc6 no higher than ${CPACK_VIBE_MAX_LIBC}) —\n"
            "  every library is linked in\n"
            "  (vibeserver/CMakeLists.txt, VIBE_LINUX_DEPS). Something linked a system library;\n"
            "  `readelf -d` the binaries for the NEEDED entry. The package has been DELETED.")
  endif()
  message(STATUS "check-deb-depends: ${_deb}: Depends: ${_dep} — ok")
endforeach()
