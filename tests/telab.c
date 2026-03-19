/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* telab.c -- Elaboration tests
 * Proving that parameters are more than just suggestions
 * and widths are more than just opinions. */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* ---- Helper ---- */
static void elab_str(const char *src, tk_lex_t *L,
                     tk_parse_t *P, ce_val_t **cv)
{
    char *pp;
    uint32_t pplen, slen;

    slen = (uint32_t)strlen(src);
    pp = (char *)malloc(slen * 2 + 256);
    if (!pp) return;
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);
    tk_pinit(P, L);
    tk_parse(P);
    *cv = (ce_val_t *)calloc(P->n_node, sizeof(ce_val_t));
    if (*cv) {
        ce_eval(P, *cv, P->n_node);
        el_elab(P, *cv, P->n_node);
    }
}

/* ---- Constant evaluation ---- */

static void ce_arith(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    ce_val_t *cv = NULL;
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    elab_str(
        "module test #(parameter A = 3, parameter B = 4)(\n"
        "  output logic [A+B-1:0] q);\n"
        "endmodule", L, P, &cv);
    CHECK(cv != NULL);
    CHECK(P->n_err == 0);

    free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("elab", ce_arith)

static void ce_clog2(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    ce_val_t *cv = NULL;
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    elab_str(
        "module test #(parameter DEPTH = 16)();\n"
        "  localparam ADDR_W = $clog2(DEPTH);\n"
        "endmodule", L, P, &cv);
    CHECK(cv != NULL);
    CHECK(P->n_err == 0);

    free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("elab", ce_clog2)

/* ---- Width inference ---- */

static void wi_basic(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    ce_val_t *cv = NULL;
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    elab_str(
        "module test(input logic [7:0] a, output logic [15:0] b);\n"
        "endmodule", L, P, &cv);
    CHECK(cv != NULL);
    CHECK(P->n_err == 0);

    wi_val_t *wv = (wi_val_t *)calloc(P->n_node, sizeof(wi_val_t));
    CHECK(wv != NULL);
    int nw = wi_eval(P, cv, P->n_node, wv, P->n_node);
    CHECK(nw > 0);

    free(wv); free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("elab", wi_basic)

/* ---- Full pipeline on smoke.sv ---- */

static void el_smoke(void)
{
    char obuf[TH_BUFSZ];
    (void)th_run(TK_BIN " --parse tests/smoke.sv", obuf, TH_BUFSZ);

    CHECK(strstr(obuf, "WIDTH = 8") != NULL);
    CHECK(strstr(obuf, "N = 32") != NULL);
    CHECK(strstr(obuf, "count: 8 bits") != NULL);
    CHECK(strstr(obuf, "result: 32 bits") != NULL);
    CHECK(strstr(obuf, "zero: 1 bits") != NULL);
    PASS();
}
TH_REG("elab", el_smoke)

static void el_fifo(void)
{
    char obuf[TH_BUFSZ];
    (void)th_run(TK_BIN " --parse tests/bigger.sv", obuf, TH_BUFSZ);

    CHECK(strstr(obuf, "DEPTH = 16") != NULL);
    CHECK(strstr(obuf, "WIDTH = 8") != NULL);
    CHECK(strstr(obuf, "ADDR_W = 4") != NULL);
    CHECK(strstr(obuf, "wr_data: 8 bits") != NULL);
    CHECK(strstr(obuf, "wr_ptr: 5 bits") != NULL);
    CHECK(strstr(obuf, "branches pruned") != NULL);
    PASS();
}
TH_REG("elab", el_fifo)
