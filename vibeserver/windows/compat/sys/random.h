/* ★ Windows stand-in for <sys/random.h>: getrandom() from the system CSPRNG (BCryptGenRandom), which is
 *   what the core needs it for — session tokens and admin tickets, never a userspace PRNG. */
#pragma once
#include "vibe_win_compat.h"
#include <bcrypt.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM   2
static inline ssize_t getrandom(void* buf, size_t n, unsigned flags) {
    (void)flags;
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) { errno = EIO; return -1; }
    return (ssize_t)n;
}
