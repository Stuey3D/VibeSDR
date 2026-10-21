# SDRPLAY_SANITIZE: "" (off), "address;undefined", "thread", "undefined", ...
# Applied globally so that static libraries, the shared client library, the
# daemon and the tests are all instrumented consistently.
set(SDRPLAY_SANITIZE "" CACHE STRING "Sanitizers to enable (e.g. address;undefined or thread)")

if(SDRPLAY_SANITIZE)
  if(MSVC)
    if("address" IN_LIST SDRPLAY_SANITIZE)
      add_compile_options(/fsanitize=address)
    endif()
  else()
    if("thread" IN_LIST SDRPLAY_SANITIZE AND "address" IN_LIST SDRPLAY_SANITIZE)
      message(FATAL_ERROR "SDRPLAY_SANITIZE: thread and address cannot be combined")
    endif()
    string(REPLACE ";" "," _san "${SDRPLAY_SANITIZE}")
    add_compile_options(-fsanitize=${_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${_san})
    message(STATUS "Sanitizers: ${_san}")
  endif()
endif()

# TSan on kernels with vm.mmap_rnd_bits=32 (Ubuntu 24.04) aborts with
# "unexpected memory mapping"; every test executable of a TSan build is run
# through this launcher (ASLR disabled). Empty otherwise.
set(SDRPLAY_TEST_LAUNCHER "")
if("thread" IN_LIST SDRPLAY_SANITIZE)
  find_program(SETARCH setarch)
  if(SETARCH)
    set(SDRPLAY_TEST_LAUNCHER ${SETARCH} -R)
  endif()
endif()
