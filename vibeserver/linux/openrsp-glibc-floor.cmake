# openrsp-glibc-floor.cmake — included into the bundled OpenRSP build (CMAKE_PROJECT_INCLUDE), self-contained
# Linux builds only. See the OpenRSP block in vibeserver/CMakeLists.txt.
#
# ★★★ THE SAME glibc 2.35 FLOOR AS THE SERVER. OpenRSP links libstdc++ statically into its library and
#     service (SDRPLAY_STATIC_CXX_RUNTIME), and bookworm's libstdc++.a calls arc4random@GLIBC_2.36 — the
#     first test build came out needing 2.36 in both, which would refuse Ubuntu 22.04 exactly as the server
#     once did. linux/glibc_floor.c defines arc4random from getentropy(); this puts it into every target the
#     OpenRSP tree creates after project(). In the library the version script keeps it local (not exported).
# ★ Kept OUT of vendor/openrsp on purpose: it is our packaging rule, not a change to the driver.
add_library(vibe_glibc_floor OBJECT "${CMAKE_CURRENT_LIST_DIR}/glibc_floor.c")
set_target_properties(vibe_glibc_floor PROPERTIES POSITION_INDEPENDENT_CODE ON)
link_libraries(vibe_glibc_floor)
