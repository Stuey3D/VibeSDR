# Public API headers.
#
# Default: OpenRSP's own headers in include/ (written for this project,
# interface- and ABI-identical to the SDRplay API 3.15 headers). The build needs
# no SDRplay file. SDRPLAY_API_HEADER_DIR may point at SDRplay's API 3.15 headers
# instead (e.g. -DSDRPLAY_API_HEADER_DIR=/usr/local/include); the build output is
# the same either way.
set(SDRPLAY_API_HEADER_DIR "${PROJECT_SOURCE_DIR}/include" CACHE PATH "Directory containing sdrplay_api.h")
if(NOT EXISTS "${SDRPLAY_API_HEADER_DIR}/sdrplay_api.h")
  message(FATAL_ERROR "sdrplay_api.h not found in SDRPLAY_API_HEADER_DIR=${SDRPLAY_API_HEADER_DIR}")
endif()
add_library(sdrplay_api_headers INTERFACE)
# SYSTEM: keeps external headers (which may not be -Wpedantic clean) usable under our warning policy.
target_include_directories(sdrplay_api_headers SYSTEM INTERFACE "${SDRPLAY_API_HEADER_DIR}")
