# Warning policy: warning-free with GCC and Clang at -Wall -Wextra -Wpedantic,
# MSVC at /W4. SDRPLAY_WERROR=ON turns warnings into errors (CI default).
function(sdrplay_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(SDRPLAY_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align
      -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
      -Wformat=2 -Wvla)
    if(SDRPLAY_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
