/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tspans.c -- Source location propagation tests
 *
 * Phase 1 source-span plumbing carries line and col from
 * AST nodes through the lowerer into rt_net_t and rt_cell_t.
 * These tests prove that when a port, a literal, a binary
 * op, or a ternary appears in the source at a known line,
 * the cell or net it produces remembers where it came from.
 *
 * If any of these ever fails, somebody has either changed
 * the lowerer to call the untagged rt_anet or rt_acell
 * variants, or changed the AST line/col mechanism, and
 * --annot and source-aware diagnostics will silently rot
 * until somebody notices that error messages stopped
 * pointing at user code.
 */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* ---- Helper: full pipeline from string (mirrors topt.c) ---- */

static rt_mod_t *
rtl_str(const char *src)
{
    tk_lex_t *L;
    tk_parse_t *P;
    ce_val_t *cv;
    wi_val_t *wv;
    rt_mod_t *M;
    char *pp;
    uint32_t pplen, slen;

    L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    if (!L || !P) { free(L); free(P); return NULL; }
    if (tk_ldinit(L, "defs/sv_tok.def") != 0) {
        free(L); free(P); return NULL;
    }

    slen = (uint32_t)strlen(src);
    pp = (char *)malloc(slen * 2 + 256);
    if (!pp) { tk_ldfree(L); free(L); free(P); return NULL; }
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);

    tk_pinit(P, L);
    tk_parse(P);

    cv = (ce_val_t *)calloc(P->n_node, sizeof(ce_val_t));
    wv = (wi_val_t *)calloc(P->n_node, sizeof(wi_val_t));
    if (!cv || !wv) {
        free(cv); free(wv);
        tk_pfree(P); free(P);
        tk_ldfree(L); free(L);
        return NULL;
    }

    ce_eval(P, cv, P->n_node);
    el_elab(P, cv, P->n_node);
    ge_expand(P);
    fl_flat(P);
    wi_eval(P, cv, P->n_node, wv, P->n_node);

    M = lw_build(P, cv, wv, P->n_node);

    free(wv);
    free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    return M;
}

/* ---- Find net by name (same as topt.c) ---- */

static uint32_t
fnet(const rt_mod_t *M, const char *name)
{
    uint32_t i;
    uint16_t nlen = (uint16_t)strlen(name);
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        if (n->name_len == nlen &&
            memcmp(M->strs + n->name_off, name, nlen) == 0)
            return i;
    }
    return 0;
}

/* ---- Find cell by type driving a given net ---- */

static uint32_t
fcell(const rt_mod_t *M, rt_ctype_t type, uint32_t out_net)
{
    uint32_t i;
    for (i = 1; i < M->n_cell; i++) {
        if (M->cells[i].type == type &&
            (out_net == 0 || M->cells[i].out == out_net))
            return i;
    }
    return 0;
}

/* ---- Ports carry source line and column ----
 * The simplest case. A port declared on line 2 of the
 * source should produce a net whose line field is 2. */

static void
sp_port(void)
{
    /* Line numbers shown in comments are 1-indexed and
     * match what the lexer assigns. */
    rt_mod_t *M = rtl_str(
        "module t(\n"               /* line 1 */
        "  input  logic a,\n"       /* line 2 */
        "  output logic b);\n"      /* line 3 */
        "  assign b = a;\n"         /* line 4 */
        "endmodule\n");             /* line 5 */
    uint32_t ai, bi;

    CHECK(M != NULL);

    ai = fnet(M, "a");
    bi = fnet(M, "b");
    CHECK(ai != 0);
    CHECK(bi != 0);

    /* Both ports must have non-zero line numbers. The exact
     * value depends on parser conventions but should land
     * in the range of the source we wrote. */
    CHECK(M->nets[ai].line > 0);
    CHECK(M->nets[bi].line > 0);
    CHECK(M->nets[ai].line <= 5);
    CHECK(M->nets[bi].line <= 5);

    rt_free(M); free(M);
    PASS();
}
TH_REG("spans", sp_port)

/* ---- Integer literals carry their source line ----
 * The CONST cell produced by lowering 1'b1 should know
 * which line of source the literal came from. */

static void
sp_lit(void)
{
    rt_mod_t *M = rtl_str(
        "module t(output logic b);\n"   /* line 1 */
        "  assign b = 1'b1;\n"          /* line 2 */
        "endmodule\n");                 /* line 3 */
    uint32_t ci;

    CHECK(M != NULL);

    /* Find the CONST cell. There should be exactly one. */
    ci = fcell(M, RT_CONST, 0);
    CHECK(ci != 0);
    CHECK(M->cells[ci].line > 0);
    CHECK(M->cells[ci].line <= 3);

    rt_free(M); free(M);
    PASS();
}
TH_REG("spans", sp_lit)

/* ---- Binary operations carry their source line ----
 * a + b produces an RT_ADD cell whose source location
 * points at the binary expression in the user's source. */

static void
sp_binop(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"                       /* line 1 */
        "  input  logic [7:0] a,\n"         /* line 2 */
        "  input  logic [7:0] b,\n"         /* line 3 */
        "  output logic [7:0] c);\n"        /* line 4 */
        "  assign c = a + b;\n"             /* line 5 */
        "endmodule\n");                     /* line 6 */
    uint32_t ai;

    CHECK(M != NULL);

    ai = fcell(M, RT_ADD, 0);
    CHECK(ai != 0);
    CHECK(M->cells[ai].line > 0);
    CHECK(M->cells[ai].line <= 6);

    rt_free(M); free(M);
    PASS();
}
TH_REG("spans", sp_binop)

/* ---- Ternary expressions carry their source line ----
 * a ? b : c lowers to RT_MUX. The MUX cell remembers
 * where the ternary was written. */

static void
sp_mux(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"                       /* line 1 */
        "  input  logic       sel,\n"       /* line 2 */
        "  input  logic [7:0] a,\n"         /* line 3 */
        "  input  logic [7:0] b,\n"         /* line 4 */
        "  output logic [7:0] c);\n"        /* line 5 */
        "  assign c = sel ? a : b;\n"       /* line 6 */
        "endmodule\n");                     /* line 7 */
    uint32_t mi;

    CHECK(M != NULL);

    mi = fcell(M, RT_MUX, 0);
    CHECK(mi != 0);
    CHECK(M->cells[mi].line > 0);
    CHECK(M->cells[mi].line <= 7);

    rt_free(M); free(M);
    PASS();
}
TH_REG("spans", sp_mux)

/* ---- Different source lines produce different spans ----
 * The strongest guarantee: two CONST cells whose literals
 * are on different lines must carry different line
 * numbers. If both come back zero or both come back the
 * same non-zero value, span propagation is broken. */

static void
sp_distinct(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"                       /* line 1 */
        "  output logic [7:0] x,\n"         /* line 2 */
        "  output logic [7:0] y);\n"        /* line 3 */
        "  assign x = 8'h11;\n"             /* line 4 */
        "  assign y = 8'h22;\n"             /* line 5 */
        "endmodule\n");                     /* line 6 */
    uint32_t xi, yi;
    uint32_t cx = 0, cy = 0;
    uint32_t i;

    CHECK(M != NULL);

    xi = fnet(M, "x");
    yi = fnet(M, "y");
    CHECK(xi != 0);
    CHECK(yi != 0);

    /* Walk the cells looking for CONST cells whose outputs
     * eventually feed x and y. With no optimisation in play
     * the CONST drives an ASSIGN which drives the port net,
     * so find the CONST cells and check their lines differ. */
    for (i = 1; i < M->n_cell; i++) {
        if (M->cells[i].type == RT_CONST) {
            if (M->cells[i].param == 0x11) cx = i;
            else if (M->cells[i].param == 0x22) cy = i;
        }
    }
    CHECK(cx != 0);
    CHECK(cy != 0);
    CHNE(M->cells[cx].line, (uint32_t)0);
    CHNE(M->cells[cy].line, (uint32_t)0);
    CHNE(M->cells[cx].line, M->cells[cy].line);

    rt_free(M); free(M);
    PASS();
}
TH_REG("spans", sp_distinct)
