/* ★ Windows stand-in for <dlfcn.h>: LoadLibrary / GetProcAddress under the POSIX names, for the few
 *   libraries the server loads at run time (the SDRplay API is the owner's own install, never linked).
 * ★★ Paths are UTF-8, as everywhere in the core, and converted to UTF-16 here — the A-functions would read
 *    them in the ANSI code page and fail on an owner whose user folder has an accented name. */
#pragma once
#include "vibe_win_compat.h"
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0
#define RTLD_NOLOAD 4
#define RTLD_DEFAULT ((void*)0)
static __thread char vibe_dlerr_[160];
static inline void* dlopen(const char* path, int flags) {
    if (!path) return (void*)GetModuleHandleW(NULL);
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, (int)(sizeof w / sizeof w[0]))) return NULL;
    HMODULE h = (flags & RTLD_NOLOAD) ? GetModuleHandleW(w)
              : LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!h) snprintf(vibe_dlerr_, sizeof vibe_dlerr_, "LoadLibrary(%s) failed, error %lu", path, GetLastError());
    return (void*)h;
}
static inline void* dlsym(void* h, const char* name) {
    FARPROC p = GetProcAddress(h ? (HMODULE)h : GetModuleHandleW(NULL), name);
    if (!p) snprintf(vibe_dlerr_, sizeof vibe_dlerr_, "%s not found", name);
    return (void*)p;
}
static inline int dlclose(void* h) { return FreeLibrary((HMODULE)h) ? 0 : -1; }
static inline char* dlerror(void) { if (!vibe_dlerr_[0]) return NULL; static __thread char e[160]; memcpy(e, vibe_dlerr_, sizeof e); vibe_dlerr_[0] = 0; return e; }
