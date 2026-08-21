/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tadv.c -- adversarial input
 *
 * Everything here asks the same question. When a bound is reached, does the
 * tool say so, or does it hand back a smaller answer and call it a success?
 *
 * The pools are runtime fields as well as compile-time constants, so a test
 * shrinks max_tok to eight rather than generating two megabytes of source to
 * reach the real one. That is the only reason these paths had never been
 * exercised. */

#include "tharns.h"
#include "takahe.h"

/* Lex a string with the pools already tampered with by the caller. */
static int
ad_lex(const char *src, tk_lex_t *L)
{
    char *pp;
    uint32_t pplen, slen;

    slen = (uint32_t)strlen(src);
    pp = (char *)malloc((size_t)slen * 2 + 256);
    if (!pp) return -1;
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);
    return (int)L->n_tok;
}

/* ---- Token pool ----
 * Past the limit the lexer used to return quietly, so the source truncated
 * and the parser made what it could of the remains. */

static void ad_tokp(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    L->max_tok = 8;
    ad_lex("module m; wire a; wire b; wire c; wire d; endmodule", L);

    CHECK(L->n_tok == 8);       /* filled it exactly, no overrun */
    CHECK(L->tok_ovf == 1);     /* and noticed */
    CHECK(L->n_err > 0);        /* and it reaches the exit status */

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("adv", ad_tokp)

/* Reported once, however many tokens are left over. */
static void ad_tok1(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    L->max_tok = 4;
    ad_lex("module m; wire a; wire b; wire c; wire d; wire e; endmodule", L);

    CHECK(L->n_err == 1);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("adv", ad_tok1)

/* ---- String pool ----
 * Worse than truncation. lx_sint returns 0 when the pool is full and 0 is a
 * real offset, so every later identifier aliases onto the first one interned
 * and the netlist comes out plausible and wrong. */

static void ad_strp(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    L->str_max = 24;
    ad_lex("module alpha; wire bravo; wire charlie; endmodule", L);

    CHECK(L->n_err > 0);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("adv", ad_strp)

/* Distinct identifiers must not collapse onto one another. */
static void ad_alias(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    uint32_t i, first = 0;
    int distinct = 0;

    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    L->str_max = 24;
    ad_lex("module alpha; wire bravo; wire charlie; endmodule", L);

    for (i = 0; i < L->n_tok; i++)
        if (L->tokens[i].type == TK_TOK_IDENT) {
            if (distinct++ == 0) first = L->tokens[i].off;
            else if (L->tokens[i].off != first) distinct = 99;
        }

    /* Either every identifier landed somewhere different, or the run failed.
     * Silently pointing them all at offset 0 is the one outcome barred. */
    CHECK(distinct == 99 || L->n_err > 0);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("adv", ad_alias)

/* ---- AST node pool ---- */

static void ad_nodp(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    char *pp;
    uint32_t pplen;
    const char *src = "module m; wire a, b, c, d, e; "
                      "assign a = b & c | d ^ e; endmodule";
    uint32_t slen = (uint32_t)strlen(src);

    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    pp = (char *)malloc((size_t)slen * 2 + 256);
    CHECK(pp != NULL);
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);

    tk_pinit(P, L);
    P->max_node = 6;
    tk_parse(P);

    CHECK(P->n_node <= 6);      /* stayed inside the pool */
    CHECK(P->n_err > 0);        /* and said the design did not fit */

    tk_pfree(P); tk_ldfree(L);
    free(P); free(L);
    PASS();
}
TH_REG("adv", ad_nodp)

/* ---- Guard sized in the wrong units ----
 * The main lexer loop was bounded by TK_MAX_TOKENS while iterating over
 * characters, so whitespace spent budget that produced no token. This source
 * is mostly whitespace and longer than the old bound. Before the fix it
 * stopped two thirds of the way through and returned success. */

static void ad_guard(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    const uint32_t pad = 1200000;
    char *src;
    uint32_t i;

    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    src = (char *)malloc(pad + 64);
    CHECK(src != NULL);
    for (i = 0; i < pad; i++) src[i] = (i % 40 == 39) ? '\n' : ' ';
    memcpy(src + pad, "module tail; endmodule", 23);

    ad_lex(src, L);
    free(src);

    /* The whole file, or an error. Not a quiet two thirds of it. */
    CHECK(L->n_err == 0);
    CHECK(L->n_tok >= 4);
    CHECK(L->tokens[L->n_tok - 1].type == TK_TOK_EOF);
    CHECK(L->tokens[L->n_tok - 1].line > 29000);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("adv", ad_guard)

/* ---- Rubbish in ----
 * Bytes no SystemVerilog source would contain. Errors are the right answer,
 * a hang or a crash is not. */

static void ad_junk(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    char junk[512];
    uint32_t i;

    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    /* Fixed pattern rather than random, so a failure reproduces. */
    for (i = 0; i < sizeof junk - 1; i++)
        junk[i] = (char)(1 + ((i * 37 + 11) % 254));
    junk[sizeof junk - 1] = '\0';

    ad_lex(junk, L);
    CHECK(L->n_err > 0);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("adv", ad_junk)

/* Unterminated everything. Each of these ends mid-construct. */
static void ad_trunc(void)
{
    static const char *cases[] = {
        "module m;",
        "module m; wire a = \"unterminated",
        "/* unterminated comment",
        "module m; assign a = (((((",
        "`ifdef NOPE",
        "module m; wire [",
        ""
    };
    uint32_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
        CHECK(L != NULL);
        CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);
        ad_lex(cases[i], L);          /* must return at all */
        CHECK(L->n_tok <= L->max_tok);
        tk_ldfree(L); free(L);
    }
    PASS();
}
TH_REG("adv", ad_trunc)

/* Nesting deeper than anything sane, to prove the bound is a bound. */
static void ad_deep(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    char *src;
    uint32_t i, n = 2000;

    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    src = (char *)malloc(n * 2 + 64);
    CHECK(src != NULL);
    memcpy(src, "module m; assign a = ", 21);
    for (i = 0; i < n; i++) src[21 + i] = '(';
    src[21 + n] = 'b';
    for (i = 0; i < n; i++) src[22 + n + i] = ')';
    memcpy(src + 22 + n + n, "; endmodule", 12);

    ad_lex(src, L);
    free(src);

    tk_pinit(P, L);
    tk_parse(P);                  /* must terminate, verdict is its own */
    CHECK(P->n_node <= P->max_node);

    tk_pfree(P); tk_ldfree(L);
    free(P); free(L);
    PASS();
}
TH_REG("adv", ad_deep)
