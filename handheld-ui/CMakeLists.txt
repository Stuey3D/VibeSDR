# vibe-hhui — the R36S native receiver UI (docs/v12/R36S-NATIVE-UI.md). Linux only: SDL2 (KMSDRM) + OpenGL ES 3.
cmake_minimum_required(VERSION 3.16)
project(vibe-hhui CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2 SDL2_ttf glesv2)
find_package(Threads REQUIRED)
# ★ The app's waterfall + spectrum, ported (src/wf/README.md) — bit-exact against the app's own TS in test/.
file(GLOB WF_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/wf/*.cpp)
# ★ The app's controller keyboard (layouts + editing rules), ported (src/kb/README.md) — golden-tested against the TS.
file(GLOB KB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/kb/*.cpp)
add_executable(vibe-hhui src/main.cpp ${WF_SOURCES} ${KB_SOURCES})
target_include_directories(vibe-hhui PRIVATE src ${SDL2_INCLUDE_DIRS})
target_link_libraries(vibe-hhui PRIVATE ${SDL2_LIBRARIES} Threads::Threads)
# ★★ -ffp-contract=off: no fused multiply-adds — JS never fuses, and fusing moved ring and palette bytes off the app's
target_compile_options(vibe-hhui PRIVATE -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)
install(TARGETS vibe-hhui RUNTIME DESTINATION bin)
