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
//  ★ Async-signal-safe throughout bar putWhere's dladdr and backtrace(), which is primed once at install so its
//    first-use dlopen of libgcc_s cannot happen inside the handler. ★★ ORDER MATTERS on a corrupt heap (2026-11-03):
//    the header, the stack scan (raw syscalls only), the library line, the registers, and backtrace() LAST — it can
//    malloc, and a USRP clone's report died inside it before the scan had printed. The handler runs ONCE and then gets out of
//    the way: SA_RESETHAND restores the default action, so systemd still sees status=11/SEGV (or 6/ABRT)
//    and a core is still written wherever the box allows one.
//  ★ Costs nothing until a fatal signal: no thread, no timer, no polling.
#pragma once

#if defined(__linux__)
#include <csignal>
#include <dlfcn.h>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <execinfo.h>
#include <initializer_list>
#include <link.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

namespace vibecrash {

inline uintptr_t& exeBase() { static uintptr_t b = 0; return b; }
/* ★★ THE EXECUTABLE'S CODE RANGE (2026-11-03, the first real USRP: a fault inside libc, from the 'vibe-usrp' thread,
 *  and backtrace() printed NOTHING — the build links libgcc statically, so glibc's backtrace has no unwinder to load).
 *  Known at install; the stack scan below keeps only words that point into it. */
inline uintptr_t& exeTextLo() { static uintptr_t v = 0; return v; }
inline uintptr_t& exeTextHi() { static uintptr_t v = 0; return v; }
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

/* ★★ WHICH LOADED OBJECT AN ADDRESS IS IN — "libc.so.6+0x…" (the offset inside it, symbolised with the box's own
 *  libc: `gdb -batch -ex 'info symbol 0x…' /lib/x86_64-linux-gnu/libc.so.6`) and, where the object exports one, the
 *  nearest symbol's name via dladdr. Not strictly async-signal-safe; this is the last thing the process does. */
struct ModHit { uintptr_t a; uintptr_t base; const char* name; };
inline int modCb(struct dl_phdr_info* info, size_t, void* vp) {
    ModHit* h = static_cast<ModHit*>(vp);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        const uintptr_t lo = uintptr_t(info->dlpi_addr + ph.p_vaddr), hi = lo + ph.p_memsz;
        if (h->a >= lo && h->a < hi) { h->base = uintptr_t(info->dlpi_addr); h->name = info->dlpi_name; return 1; }
    }
    return 0;
}
inline void putWhere(const char* label, uintptr_t a) {
    if (!a) return;
    put(label); putHex(a);
    ModHit h{ a, 0, nullptr };
    ::dl_iterate_phdr(modCb, &h);
    if (h.name) {
        put(" in "); put(h.name[0] ? h.name : "vibeserver"); put("+"); putHex(a - h.base);
        Dl_info di{};
        if (::dladdr(reinterpret_cast<void*>(a), &di) && di.dli_sname) {
            put(" <"); put(di.dli_sname); put("+"); putHex(a - uintptr_t(di.dli_saddr)); put(">");
        }
    }
    put("\n");
}

/* ★★ ONE PAGE OF OUR OWN MEMORY, COPIED WITHOUT RISK OF A FAULT. process_vm_readv on our own pid answers EFAULT for an
 *  unmapped or PROT_NONE page instead of raising SIGSEGV; a raw syscall, nothing of libc's heap involved.
 *  Returns 1 = copied, 0 = not readable, -1 = the call itself is refused here (ENOSYS / EPERM): fall back. */
inline int safeCopy(void* dst, uintptr_t src, size_t len) {
#if defined(SYS_process_vm_readv)
    struct iovec l{ dst, len }, r{ reinterpret_cast<void*>(src), len };
    const int saved = errno;
    const long got = ::syscall(SYS_process_vm_readv, (long)::getpid(), &l, 1L, &r, 1L, 0L);
    const int e = errno; errno = saved;
    if (got == (long)len) return 1;
    if (got < 0 && (e == ENOSYS || e == EPERM)) return -1;
    return 0;
#else
    (void)dst; (void)src; (void)len; return -1;
#endif
}

/* ★★ THE STACK SCAN — the frames backtrace() cannot walk: every word of the faulting thread's stack (from sp, up to
 *  16 KB) that points into OUR code, as exe+offset, nearest first. Return addresses dominate; an odd function pointer
 *  may appear too — read it as "these functions were on the way here". At most 40.
 *  ★★★ GUARDED AGAINST THE END OF THE STACK. 16 KB above sp can run past the top of a small thread stack into an
 *      unmapped page or another thread's PROT_NONE guard page, and a fault in here (SA_RESETHAND has already restored
 *      the default action) would end the report mid-scan. So each page is COPIED first (safeCopy), and the scan stops
 *      at the first page that cannot be read. Where that call is refused (a seccomp filter, an old kernel): mincore()
 *      says whether the page is mapped at all and the scan is held to 8 KB — a PROT_NONE guard page is still mapped to
 *      mincore, so that fallback can end early; accepted, it is the fallback. */
inline void scanStack(uintptr_t sp) {
    if (!sp || exeTextHi() <= exeTextLo()) return;
    put("VibeServer CRASH: stack scan (return addresses into vibeserver, nearest first):\n");
    const uintptr_t kPage = 4096;
    static uintptr_t buf[kPage / sizeof(uintptr_t)];      // ★ static: the alt stack is 64 KB, and this runs once
    uintptr_t p = sp & ~uintptr_t(sizeof(uintptr_t) - 1);
    uintptr_t end = sp + 16384;
    int shown = 0;
    bool direct = false;                                   // ★ true = the mincore fallback, reading in place
    while (p < end && shown < 40) {
        const uintptr_t pageEnd = (p | (kPage - 1)) + 1;
        const uintptr_t chunkEnd = pageEnd < end ? pageEnd : end;
        const size_t len = size_t(chunkEnd - p);
        const uintptr_t* words = nullptr;
        if (!direct) {
            const int c = safeCopy(buf, p, len);
            if (c == 1) words = buf;
            else if (c == 0) { put("  (the readable stack ends here)\n"); break; }
            else { direct = true; if (sp + 8192 < end) end = sp + 8192; continue; }
        } else {
            unsigned char vec = 0;
            if (::mincore(reinterpret_cast<void*>(p & ~(kPage - 1)), kPage, &vec) != 0) { put("  (the mapped stack ends here)\n"); break; }
            words = reinterpret_cast<const uintptr_t*>(p);
        }
        for (size_t i = 0; i < len / sizeof(uintptr_t) && shown < 40; i++) {
            const uintptr_t w = words[i];
            if (w >= exeTextLo() && w < exeTextHi()) { put("  exe+"); putHex(w - exeBase()); put("\n"); shown++; }
        }
        p = chunkEnd;
    }
    put("VibeServer CRASH: end of stack scan\n");
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
    /* ★★★ THE STACK SCAN COMES FIRST NOW (2026-11-03, a USRP clone's heap corruption). It used to follow backtrace(),
     *  and on that box the process died INSIDE backtrace() — it mallocs (and may dlopen) on a heap that was already
     *  corrupt — so the one part of the report that needs nothing from libc never printed. See scanStack(). Before
     *  putWhere too: dl_iterate_phdr takes the loader's lock, which a fault inside the loader would already hold. */
    scanStack(sp);
    putWhere("VibeServer CRASH: pc ", pc);
#if defined(__x86_64__)
    if (uc) {   // ★ the argument registers — a memcpy's dest/src/len are rdi/rsi/rdx
        const ucontext_t* u = static_cast<const ucontext_t*>(uc);
        put("VibeServer CRASH: rdi "); putHex(uintptr_t(u->uc_mcontext.gregs[REG_RDI]));
        put(" rsi "); putHex(uintptr_t(u->uc_mcontext.gregs[REG_RSI]));
        put(" rdx "); putHex(uintptr_t(u->uc_mcontext.gregs[REG_RDX]));
        put(" rcx "); putHex(uintptr_t(u->uc_mcontext.gregs[REG_RCX]));
        put(" rbp "); putHex(uintptr_t(u->uc_mcontext.gregs[REG_RBP])); put("\n");
    }
#endif
    /* ★ backtrace() LAST: it can malloc and dlopen, and on a corrupt heap it can die — everything above has printed by
     *  then. With libgcc linked statically it usually prints nothing; the scan above is the stack that matters. */
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
    // ★ …and its executable segment's range, for the stack scan.
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
            exeTextLo() = uintptr_t(info->dlpi_addr + ph.p_vaddr);
            exeTextHi() = exeTextLo() + ph.p_memsz;
        }
    }
    return 1;
}

/** Install once, early in main(). `version` must outlive the process (a string literal). */
inline void install(const char* version) {
#if defined(__SANITIZE_ADDRESS__)
    /* ★★ THE MEMORY-CHECK BUILD (vibeserver-asan, 2026-11-03) KEEPS AddressSanitizer's OWN HANDLER. ASan reports a
     *  SIGSEGV with a symbolised stack of every frame and the shadow memory around the address — more than this file
     *  can — and installing ours over it would replace that report with a shorter one. */
    (void)version;
    return;
#endif
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
