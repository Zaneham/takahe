/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tlex.c -- Lexer + preprocessor tests
 * Testing the front door before trying the hallway. */

#include "tharns.h"
#include "takahe.h"

/* ---- Helper: lex a string and return token count ---- */
static int lex_str(const char *src, tk_lex_t *L)
{
    char *pp;
    uint32_t pplen, slen;

    slen = (uint32_t)strlen(src);
    pp = (char *)malloc(slen * 2 + 256);
    if (!pp) return -1;
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);
    return (int)L->n_tok;
}

/* ---- Lexer basics ---- */

static void lx_kwd(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);
    CHECK(L->n_kwd > 200);  /* should have 247 keywords */
    CHECK(L->n_op > 50);    /* should have 65 operators */
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("lex", lx_kwd)

static void lx_toks(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    int n = lex_str("module foo; endmodule", L);
    CHECK(n > 0);
    CHECK(L->n_err == 0);
    /* module, foo, ;, endmodule, EOF = 5 tokens */
    CHECK(L->n_tok >= 5);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("lex", lx_toks)

static void lx_based(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    /* Based literals with whitespace: 32'h DEADBEEF */
    lex_str("32'h DEADBEEF", L);
    CHECK(L->n_err == 0);
    CHECK(L->tokens[0].type == TK_TOK_INT_LIT);

    /* Unbased unsized */
    lex_str("'0", L);
    CHECK(L->n_err == 0);
    CHECK(L->tokens[0].type == TK_TOK_INT_LIT);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("lex", lx_based)

static void lx_prepr(void)
{
    /* Preprocessor should strip ifdef blocks */
    char out[1024];
    uint32_t olen = 0;

    const char *src =
        "`define FOO\n"
        "`ifdef FOO\n"
        "wire a;\n"
        "`else\n"
        "wire b;\n"
        "`endif\n";

    tk_preproc(src, (uint32_t)strlen(src), out, 1024, &olen, NULL, 0);

    /* Should contain "wire a" but not "wire b" */
    CHECK(strstr(out, "wire a") != NULL);
    CHECK(strstr(out, "wire b") == NULL);
    PASS();
}
TH_REG("lex", lx_prepr)
