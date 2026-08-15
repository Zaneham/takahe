/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_data.c -- finding the .def and .txt files takahe cannot run without
 *
 * These used to be opened as "defs/sv_tok.def" and friends, relative to
 * wherever you happened to be standing. Fine in the source tree and useless
 * anywhere else, because an installed takahe could not find its own token
 * definitions.
 *
 * Resolved against the executable rather than a path baked in at compile
 * time, so an archive works unpacked anywhere and make install does not have
 * to agree with make about the prefix.
 *
 * Order is $TAKAHE_HOME, then the working directory so a source tree still
 * behaves, then next to the binary, then the FHS share directory above it.
 */

/* readlink() is POSIX, and -std=c99 asks glibc for ISO C only, so without this
 * it comes through implicitly declared and -Werror stops the build. Has to
 * precede every include, hence sitting above takahe.h. Left undefined on macOS,
 * where it hides _NSGetExecutablePath instead. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "takahe.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#define TK_DATA_SLOTS 4              /* a few live at once, e.g. en and mi */

static char tk_exedir[TK_PATH_MAX];
static char tk_argv0[TK_PATH_MAX];
static int  tk_exe_done;

/* Strip the last path component, in place. */
static void
tk_dirof(char *p)
{
    char *slash = NULL;
    char *c;
    for (c = p; *c; c++)
        if (*c == '/' || *c == '\\') slash = c;
    if (slash) *slash = '\0';
    else p[0] = '\0';
}

/* Where is this binary? Every platform hides it somewhere different, and
 * argv[0] is the last resort because a bare name on PATH tells you nothing. */
static const char *
tk_exe_dir(void)
{
    if (tk_exe_done) return tk_exedir;
    tk_exe_done = 1;
    tk_exedir[0] = '\0';

#ifdef _WIN32
    {
        DWORD n = GetModuleFileNameA(NULL, tk_exedir, (DWORD)sizeof(tk_exedir) - 1);
        if (n > 0 && n < sizeof(tk_exedir)) tk_exedir[n] = '\0';
        else tk_exedir[0] = '\0';
    }
#elif defined(__APPLE__)
    {
        uint32_t n = (uint32_t)sizeof(tk_exedir);
        if (_NSGetExecutablePath(tk_exedir, &n) != 0) tk_exedir[0] = '\0';
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", tk_exedir, sizeof(tk_exedir) - 1);
        if (n > 0) tk_exedir[(size_t)n] = '\0';
        else tk_exedir[0] = '\0';
    }
#endif

    if (!tk_exedir[0] && tk_argv0[0])
        snprintf(tk_exedir, sizeof(tk_exedir), "%s", tk_argv0);

    tk_dirof(tk_exedir);
    return tk_exedir;
}

void
tk_data_init(const char *argv0)
{
    if (argv0) snprintf(tk_argv0, sizeof(tk_argv0), "%s", argv0);
}

static int
tk_readable(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

const char *
tk_data(const char *rel)
{
    static char slot[TK_DATA_SLOTS][TK_PATH_MAX];
    static int  next;
    char *out = slot[next];
    const char *home, *dir;

    next = (next + 1) % TK_DATA_SLOTS;

    home = getenv("TAKAHE_HOME");
    if (home && home[0]) {
        snprintf(out, TK_PATH_MAX, "%s/%s", home, rel);
        if (tk_readable(out)) return out;
    }

    /* Source tree, and the historical behaviour. */
    snprintf(out, TK_PATH_MAX, "%s", rel);
    if (tk_readable(out)) return out;

    dir = tk_exe_dir();
    if (dir && dir[0]) {
        snprintf(out, TK_PATH_MAX, "%s/%s", dir, rel);
        if (tk_readable(out)) return out;

        snprintf(out, TK_PATH_MAX, "%s/../share/takahe/%s", dir, rel);
        if (tk_readable(out)) return out;
    }

    /* Nothing found. Hand back the plain name so the caller's own "cannot
     * open" carries the name the user would recognise. */
    snprintf(out, TK_PATH_MAX, "%s", rel);
    return out;
}
