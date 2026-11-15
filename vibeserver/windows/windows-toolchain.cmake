# ★ Windows (x64 first), cross-built with llvm-mingw (clang + MinGW-w64, UCRT) — see scripts/build-windows.sh.
# ★★ The triple is the only thing that changes for Windows on ARM: aarch64-w64-mingw32-clang from the
#    same image builds the NEON path vibedsp already has (docs/WINDOWS-PORT-PLAN.md §4.1).
set(CMAKE_SYSTEM_NAME Windows)
set(VIBE_WIN_DEPS "" CACHE PATH "prefix from windows/build-deps.sh")
set(VIBE_WIN_TRIPLE "x86_64-w64-mingw32" CACHE STRING "llvm-mingw target triple")
string(REGEX REPLACE "-.*" "" CMAKE_SYSTEM_PROCESSOR "${VIBE_WIN_TRIPLE}")
set(CMAKE_C_COMPILER   ${VIBE_WIN_TRIPLE}-clang)
set(CMAKE_CXX_COMPILER ${VIBE_WIN_TRIPLE}-clang++)
set(CMAKE_RC_COMPILER  ${VIBE_WIN_TRIPLE}-windres)
set(CMAKE_FIND_ROOT_PATH /opt/llvm-mingw/${VIBE_WIN_TRIPLE} ${VIBE_WIN_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
