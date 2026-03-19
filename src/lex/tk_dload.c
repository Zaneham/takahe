/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_dload.c -- Definition file loader for Takahe
 *
 * Reads sv_tok.def and populates keyword/operator tables.
 * Two-pass pattern from Tui's gr_parse.c:
 *   Pass 1: count entries, validate format
 *   Pass 2: populate tables, intern strings
 *
 * Adding a SystemVerilog keyword means adding one line to
 * sv_tok.def. This file never changes for language updates.
 * Like a customs officer who doesn't care what's in the
 * container, only that the paperwork is in order.
 *
 * JPL Power of 10: no alloc after init, bounded loops.
 */

#define KAURI_IMPL
#include "takahe.h"

#define DL_MAXLN   512    /* max line length in .def file */

/* ---- String interning ---- */

static uint32_t
dl_sint(tk_lex_t *L, const char *s, uint16_t len)
{
    uint32_t off;

    if (L->str_len + len + 1 > L->str_max) {
        return 0;  /* pool full, sentinel */
    }

    off = L->str_len;
    memcpy(L->strs + off, s, len);
    L->strs[off + len] = '\0';
    L->str_len += len + 1;

    return off;
}

/* ---- Skip whitespace, return pointer to first non-ws ---- */

static const char *
dl_skws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* ---- Extract next whitespace-delimited token ---- */

static const char *
dl_word(const char *p, const char **start, uint16_t *len)
{
    p = dl_skws(p);
    *start = p;
    *len = 0;

    while (*p && *p != ' ' && *p != '\t' &&
           *p != '\n' && *p != '\r' && *p != '#') {
        (*len)++;
        p++;
    }

    return p;
}

/* ---- Load sv_tok.def ---- */

int
tk_ldinit(tk_lex_t *L, const char *def_path)
{
    FILE *fp;
    char  line[DL_MAXLN];
    const char *p, *w1, *w2, *w3;
    uint16_t l1, l2, l3;

    if (!L || !def_path) return -1;

    memset(L, 0, sizeof(*L));

    /* Allocate string pool */
    L->strs = (char *)calloc(1, TK_MAX_STRS);
    if (!L->strs) return -1;
    L->str_max = TK_MAX_STRS;
    L->str_len = 1;  /* offset 0 = sentinel */

    /* Allocate token buffer */
    L->tokens = (tk_token_t *)calloc(TK_MAX_TOKENS, sizeof(tk_token_t));
    if (!L->tokens) { free(L->strs); return -1; }
    L->max_tok = TK_MAX_TOKENS;

    fp = fopen(def_path, "r");
    if (!fp) {
        fprintf(stderr, "takahe: cannot open def file: %s\n", def_path);
        free(L->strs);
        free(L->tokens);
        return -1;
    }

    /* Single pass: parse keywords and operators.
     * The .def format is simple enough that two passes
     * aren't needed (no forward references between entries). */
    KA_GUARD(g, 50000);
    while (g-- && fgets(line, DL_MAXLN, fp)) {
        /* Strip newline */
        int ln = (int)strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r'))
            line[--ln] = '\0';

        /* Skip empty lines and comments */
        p = dl_skws(line);
        if (*p == '\0' || *p == '#') continue;

        /* Extract directive (first word) */
        p = dl_word(p, &w1, &l1);

        if (l1 == 7 && memcmp(w1, "keyword", 7) == 0) {
            /* keyword <name> */
            p = dl_word(p, &w2, &l2);
            if (l2 == 0) continue;

            if (L->n_kwd >= TK_MAX_KWDS) continue;  /* pool full */

            tk_kwdef_t *k = &L->kwds[L->n_kwd];
            k->name_off = dl_sint(L, w2, l2);
            k->name_len = l2;
            k->id = (uint16_t)L->n_kwd;
            L->n_kwd++;
        }
        else if (l1 == 2 && memcmp(w1, "op", 2) == 0) {
            /* op <name> <chars>
             * The chars field may contain # which is also our
             * comment character. So we grab the raw chars here
             * without treating # as a comment delimiter. */
            p = dl_word(p, &w2, &l2);
            if (l2 == 0) continue;
            p = dl_skws(p);
            w3 = p;
            l3 = 0;
            while (*p && *p != ' ' && *p != '\t' &&
                   *p != '\n' && *p != '\r') {
                l3++; p++;
            }
            if (l3 == 0) continue;

            if (L->n_op >= TK_MAX_OPS) continue;

            tk_opdef_t *o = &L->ops[L->n_op];
            o->name_off  = dl_sint(L, w2, l2);
            o->name_len  = l2;
            o->chars_off = dl_sint(L, w3, l3);
            o->chars_len = l3;
            o->id = (uint16_t)L->n_op;
            L->n_op++;
        }
        /* preproc, literal, systask parsed but not yet
         * stored -- they'll get their own tables when
         * the preprocessor and parser are built. */
    }

    fclose(fp);

    /* Sort keywords for binary search during lexing.
     * Insertion sort -- fine for 233 entries, not going to
     * win any algorithm awards but it's bounded and correct. */
    {
        uint32_t i, j;
        for (i = 1; i < L->n_kwd; i++) {
            tk_kwdef_t tmp = L->kwds[i];
            j = i;
            while (j > 0) {
                const char *a = L->strs + L->kwds[j-1].name_off;
                const char *b = L->strs + tmp.name_off;
                if (strcmp(a, b) <= 0) break;
                L->kwds[j] = L->kwds[j-1];
                j--;
            }
            L->kwds[j] = tmp;
        }
        /* Reassign IDs after sort */
        for (i = 0; i < L->n_kwd; i++)
            L->kwds[i].id = (uint16_t)i;
    }

    /* Sort operators by chars length (longest first) for
     * greedy matching. Tie-break by lexicographic order. */
    {
        uint32_t i, j;
        for (i = 1; i < L->n_op; i++) {
            tk_opdef_t tmp = L->ops[i];
            j = i;
            while (j > 0) {
                tk_opdef_t *prev = &L->ops[j-1];
                /* Longer operators first (greedy match) */
                if (prev->chars_len > tmp.chars_len) break;
                if (prev->chars_len == tmp.chars_len &&
                    strcmp(L->strs + prev->chars_off,
                           L->strs + tmp.chars_off) <= 0) break;
                L->ops[j] = L->ops[j-1];
                j--;
            }
            L->ops[j] = tmp;
        }
        for (i = 0; i < L->n_op; i++)
            L->ops[i].id = (uint16_t)i;
    }

    printf("takahe: loaded %u keywords, %u operators from %s\n",
           L->n_kwd, L->n_op, def_path);

    return 0;
}

void
tk_ldfree(tk_lex_t *L)
{
    if (!L) return;
    free(L->strs);
    free(L->tokens);
    memset(L, 0, sizeof(*L));
}
