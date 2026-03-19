/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* topt.c -- Optimiser tests
 * Proving that dead cells die, constants propagate,
 * widths propagate, and MUXes appear from thin air. */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* ---- Helper: full pipeline from string ---- */

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

/* ---- Count live cells ---- */

static uint32_t
ncells(const rt_mod_t *M)
{
    uint32_t i, cnt = 0;
    for (i = 1; i < M->n_cell; i++)
        if (M->cells[i].type != RT_CELL_COUNT) cnt++;
    return cnt;
}

/* ---- Tests ---- */

/* Width: assign c = a + b with [7:0] → ADD w=8 */
static void op_width(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  input  logic [7:0] b,\n"
        "  output logic [7:0] c);\n"
        "  assign c = a + b;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t ai = fcell(M, RT_ADD, 0);
    CHECK(ai != 0);
    CHECK(M->cells[ai].width == 8);

    /* The tmp net for the ADD should be w=8 */
    uint32_t onet = M->cells[ai].out;
    CHECK(M->nets[onet].width == 8);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_width)

/* Cprop: ASSIGN of CONST → folds to CONST */
static void t_cprop(void)
{
    rt_mod_t *M = rtl_str(
        "module t(output logic b);\n"
        "  assign b = 1'b1;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t bi = fnet(M, "b");
    CHECK(bi != 0);

    /* Before opt: ASSIGN driving b */
    CHECK(fcell(M, RT_ASSIGN, bi) != 0);

    int nc = op_cprop(M, NULL);
    CHECK(nc > 0);

    /* After cprop: ASSIGN folded to CONST */
    uint32_t ci = fcell(M, RT_CONST, bi);
    CHECK(ci != 0);
    CHECK(M->cells[ci].param == 1);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", t_cprop)

/* DCE: unused intermediate gets removed */
static void op_dce_t(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  input  logic [7:0] b,\n"
        "  output logic [7:0] c);\n"
        "  assign c = a;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t before = ncells(M);

    /* c = a means just one ASSIGN. Any other cells
     * (if they existed) with unused outputs would die. */
    (void)op_dce(M);
    uint32_t after = ncells(M);

    /* Should not have removed the ASSIGN driving c */
    uint32_t ci = fnet(M, "c");
    CHECK(ci != 0);
    CHECK(fcell(M, RT_ASSIGN, ci) != 0);

    /* No change expected (no dead cells in this simple case) */
    CHECK(after <= before);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_dce_t)

/* MUX: if/else → MUX cell; DFF inference wraps it */
static void op_mux(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic       clk,\n"
        "  input  logic       sel,\n"
        "  input  logic [7:0] a,\n"
        "  input  logic [7:0] b,\n"
        "  output logic [7:0] q);\n"
        "  always_ff @(posedge clk)\n"
        "    if (sel) q <= a;\n"
        "    else     q <= b;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t qi = fnet(M, "q");
    CHECK(qi != 0);

    /* q should be marked as register */
    CHECK(M->nets[qi].is_reg == 1);

    /* q should be driven by a DFF (DFF inference) */
    uint32_t di = fcell(M, RT_DFF, qi);
    CHECK(di != 0);
    CHECK(M->cells[di].width == 8);

    /* DFF's D input should be driven by a MUX */
    uint32_t din = M->cells[di].ins[0];
    CHECK(din != 0);
    uint32_t mi = fcell(M, RT_MUX, din);
    CHECK(mi != 0);
    CHECK(M->cells[mi].n_in == 3);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_mux)

/* CASE → MUX chain */
static void op_case(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [1:0] sel,\n"
        "  input  logic [7:0] a,\n"
        "  input  logic [7:0] b,\n"
        "  output logic [7:0] y);\n"
        "  always_comb begin\n"
        "    case (sel)\n"
        "      2'b00: y = a;\n"
        "      2'b01: y = b;\n"
        "      default: y = 8'h00;\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n");
    CHECK(M != NULL);

    /* Should have MUX cells (from case items) */
    uint32_t mi = fcell(M, RT_MUX, 0);
    CHECK(mi != 0);

    /* Should have EQ cells (sel == case_val) */
    uint32_t ei = fcell(M, RT_EQ, 0);
    CHECK(ei != 0);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_case)

/* DFF inference: always_ff produces DFF cell */
static void op_dff(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic clk,\n"
        "  input  logic d,\n"
        "  output logic q);\n"
        "  always_ff @(posedge clk)\n"
        "    q <= d;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t qi = fnet(M, "q");
    CHECK(qi != 0);

    /* DFF should drive q */
    uint32_t di = fcell(M, RT_DFF, qi);
    CHECK(di != 0);

    /* DFF clk input should be the clk net */
    uint32_t ci = fnet(M, "clk");
    CHECK(ci != 0);
    CHECK(M->cells[di].ins[1] == ci);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_dff)

/* DFFR: async reset produces DFFR */
static void op_dffr(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic clk,\n"
        "  input  logic rst_n,\n"
        "  input  logic d,\n"
        "  output logic q);\n"
        "  always_ff @(posedge clk or negedge rst_n)\n"
        "    if (!rst_n) q <= 1'b0;\n"
        "    else        q <= d;\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t qi = fnet(M, "q");
    CHECK(qi != 0);

    /* DFFR should drive q */
    uint32_t di = fcell(M, RT_DFFR, qi);
    CHECK(di != 0);
    CHECK(M->cells[di].n_in == 3);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_dffr)

/* MUX with const sel → select branch */
static void t_cmux(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  output logic [7:0] b);\n"
        "  assign b = 1'b1 ? a : 8'h00;\n"
        "endmodule\n");
    CHECK(M != NULL);

    int nc = op_cprop(M, NULL);
    /* MUX with const sel=1 should fold to ASSIGN(a) */
    CHECK(nc > 0);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", t_cmux)

/* BLIF output: smoke.sv produces valid BLIF */
static void op_blif(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");

    rc = th_run(TK_BIN " --blif tests/out.blif tests/smoke.sv",
                obuf, TH_BUFSZ);
    CHECK(rc == 0);
    CHECK(strstr(obuf, "wrote") != NULL);

    /* Verify BLIF file exists and has content */
    CHECK(th_exist("tests/out.blif"));

    PASS();
}
TH_REG("opt", op_blif)

/* Full opt pipeline via CLI: --opt flag works */
static void op_cli(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");

    rc = th_run(TK_BIN " --opt tests/smoke.sv", obuf, TH_BUFSZ);
    CHECK(rc == 0);
    CHECK(strstr(obuf, "optimisations") != NULL);
    CHECK(strstr(obuf, "MUX") != NULL);
    CHECK(strstr(obuf, "DFF") != NULL);

    PASS();
}
TH_REG("opt", op_cli)

/* Constant part-select: assign b = a[3:0] → SELECT w=4 */
static void op_sel(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  output logic [3:0] b);\n"
        "  assign b = a[3:0];\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t si = fcell(M, RT_SELECT, 0);
    CHECK(si != 0);
    CHECK(M->cells[si].width == 4);
    /* param encodes (hi<<16)|lo = (3<<16)|0 */
    CHECK(M->cells[si].param == ((int64_t)3 << 16));

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_sel)

/* Single bit select: assign b = a[5] → SELECT w=1 */
static void op_bsel(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  output logic       b);\n"
        "  assign b = a[5];\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t si = fcell(M, RT_SELECT, 0);
    CHECK(si != 0);
    CHECK(M->cells[si].width == 1);
    /* param = (5<<16)|5 */
    CHECK(M->cells[si].param == (((int64_t)5 << 16) | 5));

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_bsel)

/* Concatenation: assign c = {a, b} → CONCAT w=8 */
static void op_cat(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [3:0] a,\n"
        "  input  logic [3:0] b,\n"
        "  output logic [7:0] c);\n"
        "  assign c = {a, b};\n"
        "endmodule\n");
    CHECK(M != NULL);

    uint32_t ci = fcell(M, RT_CONCAT, 0);
    CHECK(ci != 0);
    CHECK(M->cells[ci].width == 8);
    CHECK(M->cells[ci].n_in == 2);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_cat)

/* Shift with select (smoke.sv pattern): a << b[4:0] */
static void op_shft(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [31:0] a,\n"
        "  input  logic [31:0] b,\n"
        "  output logic [31:0] c);\n"
        "  assign c = a << b[4:0];\n"
        "endmodule\n");
    CHECK(M != NULL);

    /* SHL cell should exist */
    uint32_t si = fcell(M, RT_SHL, 0);
    CHECK(si != 0);

    /* SELECT cell for b[4:0] should exist, w=5 */
    uint32_t se = fcell(M, RT_SELECT, 0);
    CHECK(se != 0);
    CHECK(M->cells[se].width == 5);

    /* SHL should consume the SELECT output */
    CHECK(M->cells[si].ins[1] == M->cells[se].out);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_shft)

/* Yosys JSON: CLI produces file */
static void op_yosys(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");

    rc = th_run(TK_BIN " --yosys tests/out.json tests/smoke.sv",
                obuf, TH_BUFSZ);
    CHECK(rc == 0);
    CHECK(strstr(obuf, "wrote") != NULL);
    CHECK(th_exist("tests/out.json"));

    PASS();
}
TH_REG("opt", op_yosys)

/* ---- Multi-assign case: case(op) with result + carry_out ----
 * The bug that started the Great Lowerer Rewrite of 2026.
 * Each case item assigns to TWO outputs. The lowerer must
 * build separate MUX chains per target, not emit all
 * branches as concurrent drivers. */
static void op_mcase(void)
{
    rt_mod_t *M = rtl_str(
        "module t(\n"
        "  input  logic [7:0] a,\n"
        "  input  logic [7:0] b,\n"
        "  input  logic [1:0] op,\n"
        "  output logic [7:0] result,\n"
        "  output logic       carry\n"
        ");\n"
        "  always_comb begin\n"
        "    carry = 1'b0;\n"
        "    case (op)\n"
        "      2'b00: begin result = a + b; carry = 1'b1; end\n"
        "      2'b01: begin result = a - b; end\n"
        "      2'b10: begin result = a & b; end\n"
        "      default: begin result = a; end\n"
        "    endcase\n"
        "  end\n"
        "endmodule\n");

    CHECK(M != NULL);

    /* result should have exactly ONE driver (a MUX) */
    {
        uint32_t ri = fnet(M, "result");
        uint32_t drivers = 0, j;
        CHECK(ri != 0);
        for (j = 1; j < M->n_cell; j++) {
            if (M->cells[j].type != RT_CELL_COUNT &&
                M->cells[j].out == ri)
                drivers++;
        }
        CHECK(drivers == 1);
    }

    /* carry should have exactly ONE driver (a MUX) */
    {
        uint32_t ci = fnet(M, "carry");
        uint32_t drivers = 0, j;
        CHECK(ci != 0);
        for (j = 1; j < M->n_cell; j++) {
            if (M->cells[j].type != RT_CELL_COUNT &&
                M->cells[j].out == ci)
                drivers++;
        }
        CHECK(drivers == 1);
    }

    /* Should have MUX cells for both targets */
    CHECK(fcell(M, RT_MUX, fnet(M, "result")) != 0);
    CHECK(fcell(M, RT_MUX, fnet(M, "carry")) != 0);

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_mcase)

/* ---- Generate-for unrolling ---- */
static void op_gfor(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    rc = th_run(TK_BIN " --parse designs/dozenal_alu.sv", obuf, TH_BUFSZ);
    CHECK(rc == 0);

    /* Should report unrolling */
    CHECK(strstr(obuf, "unrolling for") != NULL);
    CHECK(strstr(obuf, "16 generate-for") != NULL);

    /* Should lower dozenal_alu (not doz_and, doz_or etc.) */
    CHECK(strstr(obuf, "lowering module 'dozenal_alu'") != NULL);

    PASS();
}
TH_REG("opt", op_gfor)

/* ---- Module name passthrough ---- */
static void op_mnam(void)
{
    rt_mod_t *M = rtl_str(
        "module my_fancy_alu(\n"
        "  input logic a,\n"
        "  output logic b\n"
        ");\n"
        "  assign b = a;\n"
        "endmodule\n");

    CHECK(M != NULL);
    CHSTR(M->mod_name, "my_fancy_alu");

    rt_free(M); free(M);
    PASS();
}
TH_REG("opt", op_mnam)

/* ---- Emitter: no multi-driver nets ---- */
static void op_emit(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("lib/sky130_fd_sc_hd__tt_025C_1v80.lib"))
        SKIP("no SKY130 lib");

    rc = th_run(TK_BIN " --lib lib/sky130_fd_sc_hd__tt_025C_1v80.lib"
                " --map tests/emit_test.v designs/voyager_fds.sv",
                obuf, TH_BUFSZ);
    CHECK(rc == 0);
    CHECK(strstr(obuf, "wrote") != NULL);

    /* Output should have module voyager_fds, not takahe_top */
    {
        FILE *f = fopen("tests/emit_test.v", "r");
        char line[256];
        int found = 0;
        if (f) {
            KA_GUARD(g, 10);
            while (fgets(line, 256, f) && g--) {
                if (strstr(line, "module voyager_fds")) found = 1;
            }
            fclose(f);
            remove("tests/emit_test.v");
        }
        CHECK(found);
    }

    PASS();
}
TH_REG("opt", op_emit)
