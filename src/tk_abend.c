/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_abend.c -- ABEND diagnostics and bilingual messages for Takahe
 *
 * Messages loaded from lang/en.txt and lang/mi.txt at startup.
 * Definition-driven: add a message by adding a line to a text
 * file, not by touching C. Same pattern as sv_tok.def for
 * keywords and cells.def for truth tables.
 *
 * When someone wants to translate Takahe to Mandarin, French,
 * or Klingon, they write a text file. The engine doesn't care
 * what language it's panicking in.
 *
 * Ko Takahe te ingoa. Ko Aotearoa te whenua.
 */

#include "takahe.h"
#include <stdarg.h>

/* ---- Language state ---- */

static int tk_lang = 0;  /* 0=en, 1=mi */

void tk_slang(int lang) { tk_lang = lang; }
int  tk_glang(void)     { return tk_lang; }

/* ---- Message pool ----
 * Fixed-size: 100 message IDs, 128 chars each.
 * Two pools: one per language loaded. */

#define TK_MSG_MAX  100
#define TK_MSG_LEN  256

static char tk_en[TK_MSG_MAX][TK_MSG_LEN];
static char tk_mi[TK_MSG_MAX][TK_MSG_LEN];
static int  tk_loaded = 0;

/* ---- Load a message file into a pool ---- */

static int
tk_lmsg(const char *path, char pool[][TK_MSG_LEN])
{
    FILE *fp;
    char line[512];
    int n = 0;

    fp = fopen(path, "r");
    if (!fp) return 0; /* not fatal — fall back to other lang */

    KA_GUARD(gln, 10000);
    while (fgets(line, 512, fp) && gln--) {
        int id;
        char *p = line;
        char *msg;

        /* Skip comments and blanks */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* Parse: TK<NNN> <message> */
        if (p[0] != 'T' || p[1] != 'K') continue;
        id = atoi(p + 2);
        if (id < 0 || id >= TK_MSG_MAX) continue;

        /* Skip past the ID to the message text */
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
        msg = p;

        /* Trim trailing newline */
        {
            int len = (int)strlen(msg);
            while (len > 0 && (msg[len-1] == '\n' || msg[len-1] == '\r'))
                msg[--len] = '\0';
        }

        /* Store — bounded copy */
        {
            int ml = (int)strlen(msg);
            if (ml >= TK_MSG_LEN) ml = TK_MSG_LEN - 1;
            memcpy(pool[id], msg, (size_t)ml);
            pool[id][ml] = '\0';
        }
        n++;
    }

    fclose(fp);
    return n;
}

/* ---- Load both language files ---- */

void
tk_linit(const char *lang_dir)
{
    char path[256];
    int ne, nm;

    if (tk_loaded) return;

    snprintf(path, 256, "%s/en.txt", lang_dir);
    ne = tk_lmsg(path, tk_en);

    snprintf(path, 256, "%s/mi.txt", lang_dir);
    nm = tk_lmsg(path, tk_mi);

    tk_loaded = 1;
    if (ne > 0 || nm > 0)
        printf("takahe: lang: %d en + %d mi messages loaded\n",
               ne, nm);
}

/* ---- Get message by ID ---- */

static const char *
tk_gmsg(int id)
{
    if (id < 0 || id >= TK_MSG_MAX) return "?";

    /* Try selected language first, fall back to English */
    if (tk_lang == 1 && tk_mi[id][0] != '\0')
        return tk_mi[id];
    if (tk_en[id][0] != '\0')
        return tk_en[id];
    return "?";
}

/* ---- Format and print a message ---- */

void
tk_emsg(int eid, ...)
{
    va_list ap;
    const char *fmt;

    if (!tk_loaded) tk_linit("lang");
    fmt = tk_gmsg(eid);
    if (!fmt || fmt[0] == '?') return;

    fprintf(stderr, "takahe TK%03d: ", eid);
    va_start(ap, eid);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ---- ABEND dump ---- */

void
tk_abend(const char *mod, const char *reason,
         const rt_mod_t *M)
{
    if (!tk_loaded) tk_linit("lang");

    fprintf(stderr, "\n");
    fprintf(stderr,
        "\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x97\n");
    fprintf(stderr,
        "\xe2\x95\x91     TAKAHE ABEND "
        "\xe2\x80\x94 HE HAPA NUKU     "
        "\xe2\x95\x91\n");
    fprintf(stderr,
        "\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x9d\n");

    /* TK090 = "Module" or "Wāhanga" */
    fprintf(stderr, "  %s:   %s\n", tk_gmsg(90),
            mod ? mod : "?");
    /* TK091 = "Reason" or "Take" */
    fprintf(stderr, "  %s:   %s\n", tk_gmsg(91),
            reason ? reason : "?");

    if (M) {
        fprintf(stderr, "  Nets:     %u / %u\n",
                M->n_net - 1, M->max_net);
        fprintf(stderr, "  Cells:    %u / %u\n",
                M->n_cell - 1, M->max_cell);
        fprintf(stderr, "  Strings:  %u / %u bytes\n",
                M->str_len, M->str_max);
        fprintf(stderr, "  Memories: %u\n", M->n_mem);
    }

    /* TK092/093 = encouragement message */
    fprintf(stderr, "\n  %s\n", tk_gmsg(92));
    fprintf(stderr, "  %s\n\n", tk_gmsg(93));
}
