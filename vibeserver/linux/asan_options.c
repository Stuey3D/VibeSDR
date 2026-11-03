/* asan_options.c — AddressSanitizer's options for the memory-check build (vibeserver-asan) ONLY.
 *
 * ★★★ BAKED IN, so the binary behaves the same however it is started: by vibeserver-maintenance `memcheck` (the admin
 *     page's "Check this radio's memory"), or by hand from a terminal. ASAN_OPTIONS in the environment still overrides
 *     any of these, one by one — the helper adds only log_path, so ASan's report also lands in a file of its own.
 *   abort_on_error=0      exit with ASan's exit code after the report, not abort() — the report is the product, and an
 *                         abort would only add a core nobody asked for (and on some boxes a crash-reporter's delay).
 *   halt_on_error=1       stop at the FIRST error: it is the one that names the author; later ones are often its echo.
 *   detect_leaks=0        a server that runs for 60 s and is then stopped is not a leak test, and LeakSanitizer's
 *                         end-of-run report would bury the line that matters. (It also needs ptrace, which the unit's
 *                         sandbox would refuse.)
 *   print_stacktrace=1    every frame of the bad access AND of where the memory was allocated and freed.
 *   symbolize=1           GCC's libasan symbolises IN PROCESS (libbacktrace) from the binary's own line tables (-g1),
 *                         so file:line appears with no llvm-symbolizer or addr2line on the box. Every frame carries
 *                         "(module+0xoffset)" as well, so an unsymbolised one is still an addr2line away:
 *                           addr2line -Cfie /usr/lib/vibeserver/diag/vibeserver-asan 0xoffset
 *   handle_abort=1        glibc's own heap checks end in abort(); ASan prints the stack for that too.
 *   malloc_context_size=30  allocation/free stacks deep enough to get past UHD's and the C++ library's frames.
 *   detect_odr_violation=0  the prebuilt static libraries (UHD, Boost) are not instrumented; an ODR "violation"
 *                         between them and us is noise here, and would halt the run before the radio opens.
 *   print_summary=1       the one-line SUMMARY the diagnostics file shows first.
 * ★ Linked only into the ASan build (CMakeLists.txt, VIBESERVER_ASAN). */
const char* __asan_default_options(void);
__attribute__((used, visibility("default"))) const char* __asan_default_options(void) {
    return "abort_on_error=0:halt_on_error=1:detect_leaks=0:print_stacktrace=1:symbolize=1:handle_abort=1"
           ":malloc_context_size=30:detect_odr_violation=0:print_summary=1";
}
