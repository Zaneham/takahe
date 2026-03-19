/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* trtl.c -- RTL IR tests
 * Proving that always_ff actually makes flip-flops
 * and not just warm feelings about clocked logic. */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* ---- Helper: lex+parse+elab+width+lower from string ---- */

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

/* ---- Find net by name ---- */

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

/* ---- Find cell by type that drives a given net ---- */

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

/* ---- Tests ---- */

/* always_ff @(posedge clk) q <= d  ->  q is_reg, ASSIGN cell */
static void rt_dff(void)
{
    rt_mod_t *M = rtl_str(
        "module t(input logic clk, input logic d,\n"
        "         output logic q);\n"
        "  always_ff @(posedge clk)\n"
        "    q <= d;\n"
        "endmodule\n");
    CHECK(M != NULL);
    CHECK(M->n_net > 1);
    CHECK(M->n_cell > 1);

    /* q should be marked as register (always_ff -> is_reg) */
    uint32_t qi = fnet(M, "q");
    CHECK(qi != 0);
    CHECK(M->nets[qi].is_reg == 1);

    /* Should have a DFF cell driving q (DFF inference) */
    uint32_t ci = fcell(M, RT_DFF, qi);
    CHECK(ci != 0);

    rt_free(M); free(M);
    PASS();
}
TH_REG("rtl", rt_dff)

/* assign c = a + b  ->  RT_ADD cell, width=8 */
static void rt_add(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  input  logic [7:0] b,\n"
        "  output logic [7:0] c);\n"
        "  assign c = a + b;\n"
        "endmodule\n");
    CHECK(M != NULL);

    /* Find an ADD cell */
    uint32_t ai = fcell(M, RT_ADD, 0);
    CHECK(ai != 0);
    CHECK(M->cells[ai].n_in == 2);

    /* Width fix: ADD cell and its output net should be w=8 */
    CHECK(M->cells[ai].width == 8);
    CHECK(M->nets[M->cells[ai].out].width == 8);

    rt_free(M); free(M);
    PASS();
}
TH_REG("rtl", rt_add)

/* assign b = a  ->  RT_ASSIGN cell */
static void rt_asgn(void)
{
    rt_mod_t *M = rtl_str(
        "module t(input logic a, output logic b);\n"
        "  assign b = a;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t bi = fnet(M, "b");
    CHECK(bi != 0);
    uint32_t ci = fcell(M, RT_ASSIGN, bi);
    CHECK(ci != 0);

    rt_free(M); free(M);
    PASS();
}
TH_REG("rtl", rt_asgn)

/* Full pipeline on smoke.sv: read file, compile, check non-zero */
static void rt_smoke(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");

    rc = th_run(TK_BIN " --parse tests/smoke.sv", obuf, TH_BUFSZ);
    CHECK(rc == 0);

    /* Should mention lowering and produce nets/cells */
    CHECK(strstr(obuf, "lowering module") != NULL);
    CHECK(strstr(obuf, "RTL:") != NULL);

    /* counter module should mark count as [reg] */
    CHECK(strstr(obuf, "[reg]") != NULL);

    PASS();
}
TH_REG("rtl", rt_smoke)

/* rt_cname returns correct names */
static void rt_names(void)
{
    CHSTR(rt_cname(RT_DFF),    "DFF");
    CHSTR(rt_cname(RT_ADD),    "ADD");
    CHSTR(rt_cname(RT_ASSIGN), "ASSIGN");
    CHSTR(rt_cname(RT_PMUX),   "PMUX");
    CHSTR(rt_cname(RT_CELL_COUNT), "???");
    PASS();
}
TH_REG("rtl", rt_names)

/* Net dedup: same signal name from different AST nodes -> one net */
static void rt_dedup(void)
{
    rt_mod_t *M = rtl_str(
        "module t(input logic a, output logic b);\n"
        "  assign b = a;\n"
        "endmodule\n");
    CHECK(M != NULL);

    /* Count nets named "a" — should be exactly 1 */
    uint32_t i, cnt = 0;
    for (i = 1; i < M->n_net; i++) {
        if (M->nets[i].name_len == 1 &&
            M->strs[M->nets[i].name_off] == 'a')
            cnt++;
    }
    CHECK(cnt == 1);

    rt_free(M); free(M);
    PASS();
}
TH_REG("rtl", rt_dedup)
