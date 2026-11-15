/* ★ Windows stand-in for <glob.h>: the one shape the core uses — a pattern whose LAST component has
 *   wildcards ("…\*\lib\x.dll" style patterns with a wildcard DIRECTORY are not matched; on Windows the
 *   callers look in fixed folders instead). FindFirstFile does the matching. */
#pragma once
#include "vibe_win_compat.h"
typedef struct { size_t gl_pathc; char** gl_pathv; size_t gl_offs; } glob_t;
#define GLOB_NOMATCH 3
static inline int glob(const char* pat, int flags, int (*errf)(const char*, int), glob_t* g) {
    (void)flags; (void)errf;
    g->gl_pathc = 0; g->gl_pathv = NULL; g->gl_offs = 0;
    const char* cut = strrchr(pat, '\\'); const char* cut2 = strrchr(pat, '/');
    if (!cut || (cut2 && cut2 > cut)) cut = cut2;
    size_t dirlen = cut ? (size_t)(cut - pat + 1) : 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return GLOB_NOMATCH;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char** nv = (char**)realloc(g->gl_pathv, (g->gl_pathc + 2) * sizeof(char*));
        if (!nv) break;
        g->gl_pathv = nv;
        size_t n = dirlen + strlen(fd.cFileName) + 1;
        char* p = (char*)malloc(n);
        if (!p) break;
        memcpy(p, pat, dirlen); strcpy(p + dirlen, fd.cFileName);
        g->gl_pathv[g->gl_pathc++] = p; g->gl_pathv[g->gl_pathc] = NULL;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return g->gl_pathc ? 0 : GLOB_NOMATCH;
}
static inline void globfree(glob_t* g) {
    for (size_t i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
    free(g->gl_pathv); g->gl_pathv = NULL; g->gl_pathc = 0;
}
