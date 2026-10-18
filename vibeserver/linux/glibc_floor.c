/* glibc_floor.c — keep the self-contained Linux build at glibc 2.35 (Ubuntu 22.04, Debian 12).
 *
 * ★★★ WHY THIS FILE EXISTS (2026-10-18). Linking libstdc++ statically (see the VIBE_LINUX_DEPS block
 *     in CMakeLists.txt) brings in the libstdc++ the BUILD image's GCC 12 was compiled against —
 *     bookworm's glibc 2.36 — and its std::random_device calls arc4random(), new in glibc 2.36.
 *     local_sdr_shim.cpp uses std::random_device, so the first self-contained package came out
 *     `libc6 (>= 2.36)` and Ubuntu 22.04 (2.35) — the very system this work was meant to open up —
 *     was refused again, by one symbol.
 *  ★ The link passes --wrap=arc4random (CMakeLists.txt), so libstdc++'s call lands here instead of on
 *    arc4random@GLIBC_2.36. getentropy() is glibc 2.25 and reads the same kernel source glibc's own
 *    arc4random() does (getrandom); random_device asks for 32 bits at a time, which never blocks
 *    after boot and is far under getentropy's 256-byte limit.
 *  ★ The 2.35 floor itself is _dl_find_object, which static libgcc's unwinder needs — fine: 22.04 has
 *    it. check-deb-depends.cmake refuses a package whose libc6 floor rises above 2.35, so the next
 *    symbol like this one cannot slip through either.
 */
#include <stdint.h>
#include <unistd.h>

uint32_t __wrap_arc4random(void);
uint32_t __wrap_arc4random(void) {
    uint32_t v = 0;
    while (getentropy(&v, sizeof v) != 0) { /* EINTR only; ENOSYS is impossible on a 3.17+ kernel */ }
    return v;
}
