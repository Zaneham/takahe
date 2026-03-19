/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_preproc.c -- Verilog/SystemVerilog preprocessor for Takahe
 *
 * Runs BEFORE the lexer on raw source text. Evaluates backtick
 * directives and outputs a single expanded buffer.
 *
 * Handles:
 *   `define NAME [value]    -- define a text macro
 *   `undef NAME             -- undefine a macro
 *   `ifdef NAME             -- conditional inclusion
 *   `ifndef NAME            -- conditional exclusion
 *   `elsif NAME             -- else-if
 *   `else                   -- else branch
 *   `endif                  -- end conditional
 *   `include "file"         -- file inclusion (future)
 *   `timescale              -- ignored (simulation only)
 *   `default_nettype        -- noted but not enforced
 *   `resetall               -- ignored
 *   Macro expansion         -- `NAME replaced by definition
 *
 * Ported from BarraCUDA's preproc.c with backtick instead of
 * hash. Same bounded-loop, no-alloc-in-hot-path philosophy.
 * Like a customs inspector reading passports — we don't care
 * what's in the luggage, just whether you're allowed in.
 *
 * JPL Power of 10: bounded loops, no recursion, fixed pools.
 */

#include "takahe.h"
#include <ctype.h>

/* ---- Limits ---- */
#define PP_MAX_MACROS  1024
#define PP_MAX_DEPTH   64      /* ifdef nesting depth */
#define PP_MAX_NAMELEN 128
#define PP_MAX_VALLEN  4096    /* max macro value length */

/* ---- Macro Table ---- */

typedef struct {
    char     name[PP_MAX_NAMELEN];
    char     value[PP_MAX_VALLEN];
    uint16_t nlen;
    uint16_t vlen;
} pp_macro_t;

typedef struct {
    /* Input */
    const char *src;
    uint32_t    src_len;
    uint32_t    pos;
    uint32_t    line;

    /* Output */
    char       *out;
    uint32_t    out_len;
    uint32_t    out_max;

    /* Macro table */
    pp_macro_t  macros[PP_MAX_MACROS];
    uint32_t    n_macro;

    /* Ifdef stack */
    int8_t      if_active[PP_MAX_DEPTH]; /* 1=emitting, 0=skipping */
    int8_t      if_seen[PP_MAX_DEPTH];   /* seen a true branch yet */
    uint32_t    if_depth;

    /* External defines (-D flags) */
    uint32_t    n_err;
} tk_pp_t;

/* ---- Character helpers ---- */

static int pp_end(const tk_pp_t *pp)
{
    return pp->pos >= pp->src_len;
}

static char pp_cur(const tk_pp_t *pp)
{
    return pp->pos < pp->src_len ? pp->src[pp->pos] : '\0';
}

static void pp_adv(tk_pp_t *pp)
{
    if (pp->pos < pp->src_len) {
        if (pp->src[pp->pos] == '\n') pp->line++;
        pp->pos++;
    }
}

/* Are we currently emitting text? (all nesting levels active) */
static int pp_eok(const tk_pp_t *pp)
{
    if (pp->if_depth == 0) return 1;
    return pp->if_active[pp->if_depth - 1];
}

/* ---- Output ---- */

static void pp_putc(tk_pp_t *pp, char c)
{
    if (pp->out_len < pp->out_max)
        pp->out[pp->out_len++] = c;
}

static void pp_puts(tk_pp_t *pp, const char *s, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        pp_putc(pp, s[i]);
}

/* ---- Macro table ---- */

static int pp_find(const tk_pp_t *pp, const char *name, uint16_t nlen)
{
    uint32_t i;
    for (i = 0; i < pp->n_macro; i++) {
        if (pp->macros[i].nlen == nlen &&
            memcmp(pp->macros[i].name, name, nlen) == 0)
            return (int)i;
    }
    return -1;
}

static void pp_def(tk_pp_t *pp, const char *name, uint16_t nlen,
                   const char *val, uint16_t vlen)
{
    int idx = pp_find(pp, name, nlen);
    pp_macro_t *m;

    if (idx >= 0) {
        m = &pp->macros[idx];
    } else {
        if (pp->n_macro >= PP_MAX_MACROS) return;
        m = &pp->macros[pp->n_macro++];
    }

    if (nlen >= PP_MAX_NAMELEN) nlen = PP_MAX_NAMELEN - 1;
    if (vlen >= PP_MAX_VALLEN) vlen = PP_MAX_VALLEN - 1;

    memcpy(m->name, name, nlen);
    m->name[nlen] = '\0';
    m->nlen = nlen;

    memcpy(m->value, val, vlen);
    m->value[vlen] = '\0';
    m->vlen = vlen;
}

static void pp_undef(tk_pp_t *pp, const char *name, uint16_t nlen)
{
    int idx = pp_find(pp, name, nlen);
    if (idx < 0) return;

    /* Swap with last to remove */
    if ((uint32_t)idx < pp->n_macro - 1)
        pp->macros[idx] = pp->macros[pp->n_macro - 1];
    pp->n_macro--;
}

/* ---- Skip to end of line ---- */

static void pp_skln(tk_pp_t *pp)
{
    while (!pp_end(pp) && pp_cur(pp) != '\n')
        pp_adv(pp);
}

/* ---- Read directive name after backtick ---- */

static uint16_t pp_rnam(tk_pp_t *pp, char *buf, uint16_t max)
{
    uint16_t len = 0;
    while (!pp_end(pp) && len < max - 1 &&
           (isalnum((unsigned char)pp_cur(pp)) || pp_cur(pp) == '_')) {
        buf[len++] = pp_cur(pp);
        pp_adv(pp);
    }
    buf[len] = '\0';
    return len;
}

/* ---- Skip whitespace (not newline) ---- */

static void pp_skws(tk_pp_t *pp)
{
    while (!pp_end(pp) && (pp_cur(pp) == ' ' || pp_cur(pp) == '\t'))
        pp_adv(pp);
}

/* ---- Read value to end of line (trimmed) ---- */

static uint16_t pp_rval(tk_pp_t *pp, char *buf, uint16_t max)
{
    uint16_t len = 0;
    pp_skws(pp);
    while (!pp_end(pp) && pp_cur(pp) != '\n' && len < max - 1) {
        buf[len++] = pp_cur(pp);
        pp_adv(pp);
    }
    /* Strip trailing // comment — `define FOO 4'h1 // note
     * should store "4'h1", not "4'h1 // note". Otherwise the
     * comment bleeds into expansions and eats your code.
     * Ask PicoRV32's case statements how they feel about this. */
    {
        uint16_t k;
        for (k = 0; k + 1 < len; k++) {
            if (buf[k] == '/' && buf[k+1] == '/') {
                len = k;
                break;
            }
        }
    }
    /* Trim trailing whitespace */
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\t'))
        len--;
    buf[len] = '\0';
    return len;
}

/* ---- Process one directive ---- */

static void pp_dir(tk_pp_t *pp)
{
    char dname[PP_MAX_NAMELEN];
    char mname[PP_MAX_NAMELEN];
    char mval[PP_MAX_VALLEN];
    uint16_t dlen, nlen, vlen;

    /* Skip the backtick */
    pp_adv(pp);

    /* Read directive name */
    dlen = pp_rnam(pp, dname, PP_MAX_NAMELEN);
    if (dlen == 0) return;

    /* ---- `define ---- */
    if (strcmp(dname, "define") == 0) {
        pp_skws(pp);
        nlen = pp_rnam(pp, mname, PP_MAX_NAMELEN);
        if (nlen == 0) { pp_skln(pp); return; }
        vlen = pp_rval(pp, mval, PP_MAX_VALLEN);
        if (pp_eok(pp))
            pp_def(pp, mname, nlen, mval, vlen);
        return;
    }

    /* ---- `undef ---- */
    if (strcmp(dname, "undef") == 0) {
        pp_skws(pp);
        nlen = pp_rnam(pp, mname, PP_MAX_NAMELEN);
        if (pp_eok(pp))
            pp_undef(pp, mname, nlen);
        pp_skln(pp);
        return;
    }

    /* ---- `ifdef ---- */
    if (strcmp(dname, "ifdef") == 0) {
        pp_skws(pp);
        nlen = pp_rnam(pp, mname, PP_MAX_NAMELEN);
        pp_skln(pp);

        if (pp->if_depth >= PP_MAX_DEPTH) return;

        int defined = pp_find(pp, mname, nlen) >= 0;
        int parent_active = pp_eok(pp);

        pp->if_active[pp->if_depth] = (int8_t)(parent_active && defined);
        pp->if_seen[pp->if_depth] = (int8_t)(defined);
        pp->if_depth++;
        return;
    }

    /* ---- `ifndef ---- */
    if (strcmp(dname, "ifndef") == 0) {
        pp_skws(pp);
        nlen = pp_rnam(pp, mname, PP_MAX_NAMELEN);
        pp_skln(pp);

        if (pp->if_depth >= PP_MAX_DEPTH) return;

        int defined = pp_find(pp, mname, nlen) >= 0;
        int parent_active = pp_eok(pp);

        pp->if_active[pp->if_depth] = (int8_t)(parent_active && !defined);
        pp->if_seen[pp->if_depth] = (int8_t)(!defined);
        pp->if_depth++;
        return;
    }

    /* ---- `elsif ---- */
    if (strcmp(dname, "elsif") == 0) {
        pp_skws(pp);
        nlen = pp_rnam(pp, mname, PP_MAX_NAMELEN);
        pp_skln(pp);

        if (pp->if_depth == 0) return;

        int defined = pp_find(pp, mname, nlen) >= 0;
        int parent_active = (pp->if_depth >= 2)
            ? pp->if_active[pp->if_depth - 2] : 1;

        if (pp->if_seen[pp->if_depth - 1]) {
            /* Already had a true branch, skip */
            pp->if_active[pp->if_depth - 1] = 0;
        } else {
            pp->if_active[pp->if_depth - 1] =
                (int8_t)(parent_active && defined);
            if (defined) pp->if_seen[pp->if_depth - 1] = 1;
        }
        return;
    }

    /* ---- `else ---- */
    if (strcmp(dname, "else") == 0) {
        pp_skln(pp);
        if (pp->if_depth == 0) return;

        int parent_active = (pp->if_depth >= 2)
            ? pp->if_active[pp->if_depth - 2] : 1;

        if (pp->if_seen[pp->if_depth - 1]) {
            pp->if_active[pp->if_depth - 1] = 0;
        } else {
            pp->if_active[pp->if_depth - 1] = (int8_t)parent_active;
            pp->if_seen[pp->if_depth - 1] = 1;
        }
        return;
    }

    /* ---- `endif ---- */
    if (strcmp(dname, "endif") == 0) {
        pp_skln(pp);
        if (pp->if_depth > 0) pp->if_depth--;
        return;
    }

    /* ---- `timescale, `default_nettype, `resetall ---- */
    if (strcmp(dname, "timescale") == 0 ||
        strcmp(dname, "default_nettype") == 0 ||
        strcmp(dname, "resetall") == 0 ||
        strcmp(dname, "celldefine") == 0 ||
        strcmp(dname, "endcelldefine") == 0 ||
        strcmp(dname, "pragma") == 0 ||
        strcmp(dname, "begin_keywords") == 0 ||
        strcmp(dname, "end_keywords") == 0 ||
        strcmp(dname, "line") == 0) {
        pp_skln(pp);
        /* Emit a newline so line numbers stay correct */
        if (pp_eok(pp)) pp_putc(pp, '\n');
        return;
    }

    /* ---- `include ---- */
    if (strcmp(dname, "include") == 0) {
        /* TODO: file inclusion. For now, skip. */
        pp_skln(pp);
        if (pp_eok(pp)) pp_putc(pp, '\n');
        return;
    }

    /* ---- Unknown directive = macro expansion ---- */
    /* `FOO expands to the value of macro FOO */
    if (pp_eok(pp)) {
        int idx = pp_find(pp, dname, dlen);
        if (idx >= 0) {
            pp_puts(pp, pp->macros[idx].value, pp->macros[idx].vlen);
        } else {
            /* Undefined macro. If followed by (, skip the
             * parenthesised arguments too — `debug(...) should
             * vanish entirely, not leave (...) in the output
             * like a ghost of debugging past. */
            if (!pp_end(pp) && pp_cur(pp) == '(') {
                int depth = 1;
                pp_adv(pp); /* skip ( */
                KA_GUARD(gm, 100000);
                while (!pp_end(pp) && depth > 0 && gm--) {
                    if (pp_cur(pp) == '(') depth++;
                    else if (pp_cur(pp) == ')') depth--;
                    pp_adv(pp);
                }
            }
            /* Non-function undefined macro: check if it's used
             * as a module name (followed by ident then '(').
             * If so, emit the name. Otherwise drop it silently.
             * `PICORV32_REGS cpuregs (...) → emit name.
             * `FORMAL_KEEP reg ... → drop, it's an attribute. */
            else {
                /* Peek ahead: skip whitespace, check for ident */
                uint32_t pk = pp->pos;
                KA_GUARD(gp, 1000);
                while (pk < pp->src_len &&
                       (pp->src[pk] == ' ' || pp->src[pk] == '\t') && gp--)
                    pk++;
                /* If next non-ws is alpha and then later '(' → module inst */
                if (pk < pp->src_len &&
                    ((pp->src[pk] >= 'a' && pp->src[pk] <= 'z') ||
                     (pp->src[pk] >= 'A' && pp->src[pk] <= 'Z') ||
                     pp->src[pk] == '_')) {
                    /* Scan past ident to see if '(' follows */
                    uint32_t pk2 = pk;
                    KA_GUARD(gp2, 1000);
                    while (pk2 < pp->src_len &&
                           ((pp->src[pk2] >= 'a' && pp->src[pk2] <= 'z') ||
                            (pp->src[pk2] >= 'A' && pp->src[pk2] <= 'Z') ||
                            (pp->src[pk2] >= '0' && pp->src[pk2] <= '9') ||
                            pp->src[pk2] == '_') && gp2--)
                        pk2++;
                    while (pk2 < pp->src_len &&
                           (pp->src[pk2] == ' ' || pp->src[pk2] == '\t'))
                        pk2++;
                    if (pk2 < pp->src_len && pp->src[pk2] == '(') {
                        /* Looks like `MOD inst (...) → emit name */
                        uint16_t k;
                        for (k = 0; k < dlen; k++)
                            pp_putc(pp, dname[k]);
                    }
                    /* else: `ATTR decl → drop silently */
                }
            }
        }
    }
}

/* ---- Main preprocessor loop ---- */

int
tk_preproc(const char *src, uint32_t src_len,
           char *out, uint32_t out_max,
           uint32_t *out_len,
           const char **defines, uint32_t n_defines)
{
    tk_pp_t *pp;
    uint32_t i;
    int rc;

    /* tk_pp_t is ~4.3MB (macro table). Stack would weep.
     * Heap-allocate like a responsible adult. */
    pp = (tk_pp_t *)calloc(1, sizeof(tk_pp_t));
    if (!pp) return -1;
    pp->src     = src;
    pp->src_len = src_len;
    pp->out     = out;
    pp->out_max = out_max;
    pp->line    = 1;

    /* Register external defines (-D flags) */
    for (i = 0; i < n_defines && i < PP_MAX_MACROS; i++) {
        const char *d = defines[i];
        const char *eq = strchr(d, '=');
        if (eq) {
            uint16_t nlen = (uint16_t)(eq - d);
            uint16_t vlen = (uint16_t)strlen(eq + 1);
            pp_def(pp, d, nlen, eq + 1, vlen);
        } else {
            uint16_t nlen = (uint16_t)strlen(d);
            pp_def(pp, d, nlen, "1", 1);
        }
    }

    /* Process source */
    KA_GUARD(g, 50000000);  /* 50M chars should be enough */
    while (!pp_end(pp) && g--) {
        char c = pp_cur(pp);

        /* ---- Line comment ---- */
        if (c == '/' && pp->pos + 1 < pp->src_len &&
            pp->src[pp->pos + 1] == '/') {
            if (pp_eok(pp)) {
                while (!pp_end(pp) && pp_cur(pp) != '\n')
                    { pp_putc(pp, pp_cur(pp)); pp_adv(pp); }
            } else {
                while (!pp_end(pp) && pp_cur(pp) != '\n')
                    pp_adv(pp);
            }
            continue;
        }

        /* ---- Block comment ---- */
        if (c == '/' && pp->pos + 1 < pp->src_len &&
            pp->src[pp->pos + 1] == '*') {
            if (pp_eok(pp)) { pp_putc(pp, '/'); pp_putc(pp, '*'); }
            pp_adv(pp); pp_adv(pp);
            KA_GUARD(gc, 5000000);
            while (!pp_end(pp) && gc--) {
                if (pp_cur(pp) == '*' && pp->pos + 1 < pp->src_len &&
                    pp->src[pp->pos + 1] == '/') {
                    if (pp_eok(pp)) { pp_putc(pp, '*'); pp_putc(pp, '/'); }
                    pp_adv(pp); pp_adv(pp);
                    break;
                }
                if (pp_eok(pp)) pp_putc(pp, pp_cur(pp));
                pp_adv(pp);
            }
            continue;
        }

        /* ---- String literal (don't expand macros inside) ---- */
        if (c == '"') {
            if (pp_eok(pp)) pp_putc(pp, c);
            pp_adv(pp);
            KA_GUARD(gs, 1000000);
            while (!pp_end(pp) && pp_cur(pp) != '"' && gs--) {
                if (pp_cur(pp) == '\\') {
                    if (pp_eok(pp)) pp_putc(pp, pp_cur(pp));
                    pp_adv(pp);
                }
                if (pp_eok(pp)) pp_putc(pp, pp_cur(pp));
                pp_adv(pp);
            }
            if (!pp_end(pp)) {
                if (pp_eok(pp)) pp_putc(pp, '"');
                pp_adv(pp);
            }
            continue;
        }

        /* ---- Backtick directive ---- */
        if (c == '`') {
            pp_dir(pp);
            continue;
        }

        /* ---- Regular character ---- */
        if (pp_eok(pp))
            pp_putc(pp, c);
        pp_adv(pp);
    }

    /* NUL-terminate */
    if (pp->out_len < pp->out_max)
        pp->out[pp->out_len] = '\0';

    if (out_len) *out_len = pp->out_len;

    if (pp->if_depth > 0) {
        fprintf(stderr, "takahe: preprocessor: %u unclosed `ifdef\n",
                pp->if_depth);
        pp->n_err++;
    }

    rc = (pp->n_err > 0) ? -1 : 0;
    free(pp);
    return rc;
}
