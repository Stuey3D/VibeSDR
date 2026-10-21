# SIMD kernel tables for the stream DSP (src/core/dsp/simd).
# Every ISA file is compiled into every build of its architecture with per-file ISA
# flags; the table is chosen at run time from the CPU features (Kernels.cpp), so the
# rest of the core stays baseline (x86-64: SSE2; armhf: VFP, NEON detected at run time).
# The SIMD files are self-contained (no shared inline/template code with other TUs), so
# no AVX2/NEON instance of a COMDAT function can leak into baseline code.
include(CheckCXXSourceCompiles)
set(_dsp_simd ${SRC}/core/dsp/simd)
check_cxx_source_compiles("#if !defined(__x86_64__) && !defined(_M_X64)\n#error no\n#endif\nint main(){return 0;}" SDRPLAY_TARGET_X86_64)
check_cxx_source_compiles("#if !defined(__arm__)\n#error no\n#endif\nint main(){return 0;}" SDRPLAY_TARGET_ARM32)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(SDRPLAY_TARGET_X86_64)
    set_source_files_properties(${_dsp_simd}/KernelsAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(${_dsp_simd}/KernelsAvx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512vl;-mavx512dq")
  elseif(SDRPLAY_TARGET_ARM32)
    set_source_files_properties(${_dsp_simd}/KernelsNeon.cpp PROPERTIES COMPILE_OPTIONS "-mfpu=neon")
  endif()
endif()
