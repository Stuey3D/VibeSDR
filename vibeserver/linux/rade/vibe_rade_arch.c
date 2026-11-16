/* vibe_rade_arch.c — which of Opus's DNN code paths RADE's neural networks run on this CPU (docs/v12/FREEDV.md, "SIMD").
 *
 * Opus 1.6.1 ships its DNN layer code (the int8 GEMV that is most of RADE's decoder and FARGAN) in several builds of one
 * source — plain C, SSE2, SSE4.1 and AVX2/FMA on x86; NEON and NEON + the int8 dot-product instruction on ARM — and picks
 * one at run time from a table indexed by an "arch" number (OPUS_HAVE_RTCD; dnn/x86/x86_dnn_map.c, dnn/arm/arm_dnn_map.c).
 * The number comes from opus_select_arch(): CPUID on x86, /proc/cpuinfo on Linux ARM, sysctl on macOS.
 *
 * ★★ rade_c never asks: rade_rx.c passes a hard-coded `int arch = 0` to its decoder, so even a dispatching build would run
 *    the decoder on the slowest table entry. build-rade.sh compiles a copy of rade_rx.c whose `arch` comes from here, and
 *    vibe_rade.c gives FARGAN the same number (fargan_init's own opus_select_arch() would ignore the override below).
 * ★ Asked once and kept: opus_select_arch() reads /proc/cpuinfo on Linux ARM, far too slow for every modem frame.
 *
 * VIBE_RADE_ARCH (TESTS ONLY — never set on a server): force a LOWER path to compare it with the best one. A number (the
 * table index) or a name: c, sse2, sse4.1, avx2 on x86; c, neon, dotprod on ARM. It is clamped to what the CPU has, so it
 * can never pick an instruction this CPU lacks; an unknown name is ignored. vrade_rx_arch_name() says what was chosen.
 *
 * Compiled with the same OPUS_* RTCD defines as Opus's DNN files (build-rade.sh's DNN_DEFS), so cpu_support.h declares the
 * real opus_select_arch(). In a build without dispatch (ARMv6, scalar) cpu_support.h makes it an inline 0 and so is this.
 *
 * Copyright (c) 2026 VibeSDR. Licensed as VibeServer (GPL-3.0).
 */
#include "vibe_rade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_support.h"

#if defined(OPUS_HAVE_RTCD) && (defined(OPUS_X86_MAY_HAVE_SSE2) || defined(OPUS_X86_MAY_HAVE_AVX2))
static const char* const kNames[5] = { "c", "sse", "sse2", "sse4.1", "avx2" };
static const char* const kFamily = "x86";
#elif defined(OPUS_HAVE_RTCD) && defined(OPUS_ARM_MAY_HAVE_NEON_INTR)
static const char* const kNames[5] = { "c", "edsp", "media", "neon", "dotprod" };
static const char* const kFamily = "arm";
#else
static const char* const kNames[5] = { "c", "c", "c", "c", "c" };
static const char* const kFamily = "scalar";
#endif

/* ★★ A SECOND OPINION, only ever lowering Opus's answer — so a wrong "yes" can never become an illegal instruction:
 *  - x86 AVX2: Opus checks CPUID only. The OS must also save the 256-bit registers (OSXSAVE, then XCR0 bits 1-2), or the
 *    first AVX instruction faults — e.g. a hypervisor or kernel (noxsave) that hides the state but not the CPUID bits.
 *  - Linux ARM: Opus reads /proc/cpuinfo's text. getauxval(AT_HWCAP) is the kernel's own answer for THIS process — the
 *    same on real hardware, but right where the text is not: under qemu-user, /proc/cpuinfo is the HOST's, so an armhf
 *    binary emulated as a Cortex-A7 on an arm64 host read "asimddp" and died on the dot product (seen 2026-11-16).
 *    32-bit: HWCAP_NEON is bit 12, HWCAP_ASIMDDP bit 24 (Linux 6.2+; older kernels never report it, and neither does
 *    their /proc/cpuinfo). 64-bit: HWCAP_ASIMDDP is bit 20. */
#if defined(OPUS_HAVE_RTCD) && (defined(OPUS_X86_MAY_HAVE_SSE2) || defined(OPUS_X86_MAY_HAVE_AVX2))
#include <cpuid.h>
static int os_clamp(int a) {
    if (a < 4) return a;
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & (1u << 27))) return 3;   /* no OSXSAVE */
    unsigned lo, hi;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));   /* xgetbv(0) */
    (void)hi;
    return (lo & 6u) == 6u ? a : 3;
}
#elif defined(OPUS_HAVE_RTCD) && defined(OPUS_ARM_MAY_HAVE_NEON_INTR) && defined(__linux__)
#include <sys/auxv.h>
static int os_clamp(int a) {
    const unsigned long hw = getauxval(AT_HWCAP);
#if defined(__aarch64__)
    if (a >= 4 && !(hw & (1ul << 20))) a = 3;
#else
    if (a >= 3 && !(hw & (1ul << 12))) a = 2;
    if (a >= 4 && !(hw & (1ul << 24))) a = 3;
#endif
    return a;
}
#else
static int os_clamp(int a) { return a; }
#endif

static int g_arch = -1;      /* -1 = not asked yet; a benign race (every thread computes the same value) */
static int g_detected = 0;

static int parse_arch(const char* s) {
    if (!s || !*s) return -1;
    if (s[0] >= '0' && s[0] <= '9') return atoi(s);
    for (int i = 0; i < 5; i++)
        if (strcmp(s, kNames[i]) == 0) return i;
    if (strcmp(s, "sse4_1") == 0) return 3;
    return -1;
}

int vrade_rx_arch(void) {
    int a = __atomic_load_n(&g_arch, __ATOMIC_ACQUIRE);
    if (a >= 0) return a;
    const int det = os_clamp(opus_select_arch());
    a = det;
    const int want = parse_arch(getenv("VIBE_RADE_ARCH"));
    if (want >= 0 && want < a) a = want;   /* ★ only ever DOWN: never an instruction the CPU lacks */
    g_detected = det;
    __atomic_store_n(&g_arch, a, __ATOMIC_RELEASE);
    return a;
}

const char* vrade_rx_arch_name(void) {
    static char buf[64];
    const int a = vrade_rx_arch();
    const char* n = (a >= 0 && a < 5) ? kNames[a] : "?";
    const char* d = (g_detected >= 0 && g_detected < 5) ? kNames[g_detected] : "?";
    if (a == g_detected) snprintf(buf, sizeof buf, "%s %s", kFamily, n);
    else snprintf(buf, sizeof buf, "%s %s (forced; this CPU has %s)", kFamily, n, d);
    return buf;
}
