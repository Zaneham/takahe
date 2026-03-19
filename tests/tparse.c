/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tparse.c -- Parser tests
 * Verifying that we understand IEEE 1800 better than our
 * therapist understands us. */

#include "tharns.h"
#include "takahe.h"

/* ---- Helper: parse a string, return node count ---- */
static int parse_str(const char *src, tk_lex_t *L, tk_parse_t *P)
{
    char *pp;
    uint32_t pplen, slen;

    slen = (uint32_t)strlen(src);
    pp = (char *)malloc(slen * 2 + 256);
    if (!pp) return -1;
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);
    if (L->n_err > 0) return -1;
    tk_pinit(P, L);
    tk_parse(P);
    return (int)P->n_node;
}

/* ---- Module parsing ---- */

static void pk_modul(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    int n = parse_str("module foo; endmodule", L, P);
    CHECK(n > 0);
    CHECK(P->n_err == 0);
    /* Root + Module = at least 2 nodes */
    CHECK(P->n_node >= 2);

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("parse", pk_modul)

static void pk_ports(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    int n = parse_str(
        "module bar(input logic [7:0] a, output logic [7:0] b);\n"
        "endmodule", L, P);
    CHECK(n > 0);
    CHECK(P->n_err == 0);

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("parse", pk_ports)

static void pk_alwff(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    int n = parse_str(
        "module dff(input logic clk, d, output logic q);\n"
        "  always_ff @(posedge clk) q <= d;\n"
        "endmodule", L, P);
    CHECK(n > 0);
    CHECK(P->n_err == 0);

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("parse", pk_alwff)

static void pk_case(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    int n = parse_str(
        "module mux(input logic [1:0] sel, output logic y);\n"
        "  always_comb case (sel)\n"
        "    2'b00: y = 1'b0;\n"
        "    2'b01: y = 1'b1;\n"
        "    default: y = 1'b0;\n"
        "  endcase\n"
        "endmodule", L, P);
    CHECK(n > 0);
    CHECK(P->n_err == 0);

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("parse", pk_case)

static void pk_param(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    CHECK(L && P);
    CHECK(tk_ldinit(L, "defs/sv_tok.def") == 0);

    int n = parse_str(
        "module cnt #(parameter WIDTH = 8)(\n"
        "  input logic clk,\n"
        "  output logic [WIDTH-1:0] q);\n"
        "  always_ff @(posedge clk) q <= q + 1'b1;\n"
        "endmodule", L, P);
    CHECK(n > 0);
    CHECK(P->n_err == 0);

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("parse", pk_param)

static void pk_smoke(void)
{
    /* Full smoke.sv file */
    char obuf[TH_BUFSZ];
    (void)th_run(TK_BIN " --parse tests/smoke.sv", obuf, TH_BUFSZ);
    CHECK(strstr(obuf, "0 errors") != NULL);
    CHECK(strstr(obuf, "150 AST nodes") != NULL);
    PASS();
}
TH_REG("parse", pk_smoke)
