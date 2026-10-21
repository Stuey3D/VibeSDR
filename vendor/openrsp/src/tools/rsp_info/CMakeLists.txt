# rsp_info: standalone read-only RSP lister.
# Portable logic (rsp_info_lib) reuses the core model table, firmware catalog and
# SHA-256 by compiling those sources directly (not sdrplay_core: no service code).
# Only UsbScan.cpp talks to libusb-1.0.
set(RSP_INFO_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(RSP_SRC ${PROJECT_SOURCE_DIR}/src)

add_library(rsp_info_lib STATIC
  ${RSP_INFO_DIR}/RspInfo.cpp
  ${RSP_SRC}/core/device/Models.cpp
  ${RSP_SRC}/core/firmware/FirmwareCatalog.cpp
  ${RSP_SRC}/core/util/Sha256.cpp)
target_include_directories(rsp_info_lib PUBLIC ${RSP_INFO_DIR} ${RSP_SRC})
target_link_libraries(rsp_info_lib PUBLIC sdrplay_api_headers)
sdrplay_set_warnings(rsp_info_lib)

if(SDRPLAY_HAVE_LIBUSB)
  add_executable(rsp_info ${RSP_INFO_DIR}/main.cpp ${RSP_INFO_DIR}/UsbScan.cpp)
  target_link_libraries(rsp_info PRIVATE rsp_info_lib PkgConfig::LIBUSB)
  sdrplay_set_warnings(rsp_info)
else()
  message(STATUS "rsp_info: libusb-1.0 not found, rsp_info is not built")
endif()

if(COMMAND sdrplay_rsp_info_tests)
  sdrplay_rsp_info_tests(${RSP_INFO_DIR})
endif()
