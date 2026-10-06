// crash_report.h — one journal record per fatal signal: what died, on which thread, and where.
//
// ★★★ WHY THIS EXISTS (2026-10-06). The Pi 2's radio process SEGFAULTED four times in one day, every
//     time while it was decoding DAB (9A twice, 12B twice, 20 s to 5 min after entering the block), on
//     rc15, rc17 and rc22 alike — and left NOTHING to read: systemd-coredump is not installed on that
//     box, core_pattern is the bare "core", and the unit's working directory is "/" under
//     ProtectSystem=strict, so the kernel's core file could not be written anywhere. The journal had
//     "status=11/SEGV" and the line before it; no thread, no address, no frame. An off-target replay
//     of the DAB service under ThreadSanitizer and AddressSanitizer (64-bit) found nothing that
//     crashes, so the next crash has to name itself.
//  ★★ What it prints is enough to symbolise with addr2line against the SAME .deb's binary (the armhf
//     package ships unstripped): the faulting PC and LR as offsets from the executable's load base,
//     the thread's NAME (vibe-dab, vibe-dab-msc, vibe-mp2, vibe-dsp … — the threads are named, so
//     this alone says which half of the receiver it was), the fault address, and the version.
//  ★ Async-signal-safe throughout bar backtrace(), which is primed once at install so its first-use
//    dlopen of libgcc_s cannot happen inside the handler. The handler runs ONCE and then gets out of
//    the way: SA_RESETHAND restores the default action, so systemd still sees status=11/SEGV (or 6/ABRT)
//    and a core is still written wherever the box allows one.
//  ★ Costs nothing until a fatal signal: no thread, no timer, no polling.
#pragma once

#if defined(__linux__)
#include <csignal>
#include <cstdint>
#include <cstring>
#include <execinfo.h>
#include <initializer_list>
#include <link.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace vibecrash {

inline uintptr_t& exeBase() { static uintptr_t b = 0; return b; }
inline const char*& versionStr() { static const char* v = "?"; return v; }

inline void put(const char* s) { if (s) { ssize_t r = ::write(2, s, std::strlen(s)); (void)r; } }
inline void putHex(uintptr_t v) {
    char b[2 + 2 * sizeof(uintptr_t) + 1]; char* p = b + sizeof b; *--p = 0;
    do { *--p = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    *--p = 'x'; *--p = '0'; put(p);
}
inline void putDec(long v) {
    char b[24]; char* p = b + sizeof b; *--p = 0;
    const bool neg = v < 0; unsigned long u = neg ? 0UL - (unsigned long)v : (unsigned long)v;
    do { *--p = char('0' + u % 10); u /= 10; } while (u);
    if (neg) *--p = '-';
    put(p);
}

/** The PC, LR (return address) and SP at the fault, per architecture. Zero where not known. */
inline void regs(void* ucv, uintptr_t& pc, uintptr_t& lr, uintptr_t& sp) {
    pc = lr = sp = 0;
    if (!ucv) return;
    const ucontext_t* uc = static_cast<const ucontext_t*>(ucv);
#if defined(__arm__)
    pc = uc->uc_mcontext.arm_pc; lr = uc->uc_mcontext.arm_lr; sp = uc->uc_mcontext.arm_sp;
#elif defined(__aarch64__)
    pc = uc->uc_mcontext.pc; lr = uc->uc_mcontext.regs[30]; sp = uc->uc_mcontext.sp;
#elif defined(__x86_64__)
    pc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]); sp = uintptr_t(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__i386__)
    pc = uintptr_t(uc->uc_mcontext.gregs[REG_EIP]); sp = uintptr_t(uc->uc_mcontext.gregs[REG_ESP]);
#else
    (void)uc;
#endif
}

/** An address as "0x… (exe+0x…)" when it lies past the executable's base — the form addr2line wants. */
inline void putAddr(const char* label, uintptr_t a) {
    put(label); putHex(a);
    if (a && exeBase() && a >= exeBase()) { put(" (exe+"); putHex(a - exeBase()); put(")"); }
}

inline void handler(int sig, siginfo_t* si, void* uc) {
    static volatile sig_atomic_t entered = 0;
    if (entered) return;              // a second thread faulting meanwhile: the first one's report stands
    entered = 1;
    char name[17] = {0};
    ::prctl(PR_GET_NAME, name, 0, 0, 0);
    uintptr_t pc, lr, sp; regs(uc, pc, lr, sp);
    put("VibeServer CRASH: signal "); putDec(sig);
    put(" code "); putDec(si ? si->si_code : 0);
    put(" fault-addr "); putHex(si ? uintptr_t(si->si_addr) : 0);
    put(" thread '"); put(name); put("' tid "); putDec(long(::syscall(SYS_gettid)));
    put(" version "); put(versionStr()); put("\n");
    put("VibeServer CRASH:"); putAddr(" pc ", pc); putAddr(" lr ", lr); put(" sp "); putHex(sp);
    put(" exe-base "); putHex(exeBase()); put("\n");
    put("VibeServer CRASH: backtrace (addr2line -Cfe /usr/bin/vibeserver <exe+offset>):\n");
    void* frames[48];
    const int n = ::backtrace(frames, 48);
    ::backtrace_symbols_fd(frames, n, 2);
    put("VibeServer CRASH: end of report\n");
    /* ★ SA_RESETHAND has put the default action back. A fault returns into the faulting instruction,
     *  which faults again and kills the process exactly as before (status=11, a core if allowed);
     *  abort() re-raises by itself. A signal SENT to us (kill, raise) does not repeat, so send it again. */
    if (!si || si->si_code <= 0) ::raise(sig);
}

inline int exeBaseCb(struct dl_phdr_info* info, size_t, void* out) {
    // ★ The first object dl_iterate_phdr reports is the executable itself (empty name).
    *static_cast<uintptr_t*>(out) = uintptr_t(info->dlpi_addr);
    return 1;
}

/** Install once, early in main(). `version` must outlive the process (a string literal). */
inline void install(const char* version) {
    versionStr() = version ? version : "?";
    uintptr_t base = 0;
    ::dl_iterate_phdr(exeBaseCb, &base);
    exeBase() = base;
    { void* prime[2]; (void)::backtrace(prime, 2); }   // ★ loads libgcc_s now, not in the handler
    // An alternate stack for the main thread, so even a stack overflow there leaves a record.
    static char alt[64 * 1024];
    stack_t ss{}; ss.ss_sp = alt; ss.ss_size = sizeof alt; ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int s : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) ::sigaction(s, &sa, nullptr);
}

}  // namespace vibecrash
#else
namespace vibecrash { inline void install(const char*) {} }
#endif
