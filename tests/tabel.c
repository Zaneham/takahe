/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tabel.c -- ABEL-HDL frontend tests
 * The last compiler for this language died around 2010.
 * The documentation is disappearing from the internet.
 * These tests prove the resurrection works. */

#include "tharns.h"
#include "takahe.h"

/* ---- Helper: lex ABEL source ---- */

static int ab_lex_str(const char *src, tk_lex_t *L)
{
    uint32_t slen = (uint32_t)strlen(src);
    ab_lex(L, src, slen);
    return (int)L->n_tok;
}

/* ---- ab_toks: basic tokenisation ---- */

static void ab_toks(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/abel_tok.def") == 0);

    int n = ab_lex_str(
        "MODULE test\n"
        "TITLE 'decoder'\n"
        "DECLARATIONS\n"
        "  A0 PIN;\n"
        "EQUATIONS\n"
        "  Y0 = !A0;\n"
        "END test\n",
        L);

    CHECK(n > 5);
    CHECK(L->n_err == 0);

    tk_ldfree(L);
    free(L);
    PASS();
}
TH_REG("abel", ab_toks)

/* ---- ab_kwds: keyword recognition (case-insensitive) ---- */

static void ab_kwds(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/abel_tok.def") == 0);

    ab_lex_str("MODULE module Module EQUATIONS equations", L);

    /* all three MODULE variants should be KWD */
    CHECK(L->tokens[0].type == TK_TOK_KWD);
    CHECK(L->tokens[1].type == TK_TOK_KWD);
    CHECK(L->tokens[2].type == TK_TOK_KWD);
    CHECK(L->tokens[3].type == TK_TOK_KWD);
    CHECK(L->tokens[4].type == TK_TOK_KWD);

    tk_ldfree(L);
    free(L);
    PASS();
}
TH_REG("abel", ab_kwds)

/* ---- ab_nums: number literals ---- */

static void ab_nums(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/abel_tok.def") == 0);

    ab_lex_str("42 ^hFF ^b1010 ^o17", L);

    CHECK(L->tokens[0].type == TK_TOK_INT_LIT);
    CHECK(L->tokens[1].type == TK_TOK_INT_LIT);
    CHECK(L->tokens[2].type == TK_TOK_INT_LIT);
    CHECK(L->tokens[3].type == TK_TOK_INT_LIT);
    CHECK(L->n_err == 0);

    tk_ldfree(L);
    free(L);
    PASS();
}
TH_REG("abel", ab_nums)

/* ---- ab_spec: special constants (.X. .C. .Z.) ---- */

static void ab_spec(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/abel_tok.def") == 0);

    ab_lex_str(".X. .C. .Z.", L);

    CHECK(L->tokens[0].type == TK_TOK_INT_LIT);
    CHECK(L->tokens[1].type == TK_TOK_INT_LIT);
    CHECK(L->tokens[2].type == TK_TOK_INT_LIT);
    CHECK(L->n_err == 0);

    tk_ldfree(L);
    free(L);
    PASS();
}
TH_REG("abel", ab_spec)

/* ---- ab_cmnt: comments (" and //) ---- */

static void ab_cmnt(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/abel_tok.def") == 0);

    ab_lex_str(
        "A0 \"this is a comment\"\n"
        "B0 // another comment\n"
        "C0\n",
        L);

    /* should get A0 B0 C0 as idents, comments skipped */
    int idents = 0;
    uint32_t t;
    for (t = 0; t < L->n_tok; t++)
        if (L->tokens[t].type == TK_TOK_IDENT) idents++;
    CHECK(idents == 3);
    CHECK(L->n_err == 0);

    tk_ldfree(L);
    free(L);
    PASS();
}
TH_REG("abel", ab_cmnt)

/* ---- ab_cli: full CLI parse of decoder.abl ---- */

static void ab_cli(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    rc = th_run(TK_BIN " --parse designs/decoder.abl", obuf, TH_BUFSZ);
    CHECK(rc == 0);
    CHECK(strstr(obuf, "AST nodes") != NULL);
    CHECK(strstr(obuf, "0 errors") != NULL);

    PASS();
}
TH_REG("abel", ab_cli)
