/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tnlst.c -- Structural netlist reading tests
 * Gates in rather than gates out, which is the whole point. */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* Both fixtures live outside the repo: the SKY130 Liberty is 12MB
 * and the netlist belongs to someone else. Tests SKIP when either
 * is missing, so a fresh clone still goes green. The netlist is the
 * warm-up from Jane Street's 2026 ASIC reverse-engineering puzzle,
 * which ships with its own source, so there's a right answer to
 * check against. */
#define SKY130_LIB "C:/dev/documents/skywater/sky130_fd_sc_hd__tt_025C_1v80.lib"
#define WARMUP "C:/dev/asic-puzzle-2026/warmup/01_netlist.v"

/* ---- Helper: lex+parse+elab+width+lower a netlist string ---- */

static rt_mod_t *
nl_str(const char *src, const lb_lib_t *lib, cd_lib_t *cd)
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

    M = lw_build_n(P, cv, wv, P->n_node, lib, cd);

    free(wv);
    free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    return M;
}

/* ---- Read a file into a malloc'd buffer ---- */

static char *
nl_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b); fclose(f); return NULL;
    }
    b[n] = '\0';
    fclose(f);
    return b;
}

static uint32_t
nl_msk(const cd_cell_t *c)
{
    uint32_t m = 0;
    uint16_t r;

    for (r = 0; r < c->n_row && r < 32; r++)
        if (c->rows[r].outs[0]) m |= 1u << r;
    return m;
}

/* Count cells of a given type */
static uint32_t
nl_cnt(const rt_mod_t *M, rt_ctype_t t)
{
    uint32_t i, n = 0;
    for (i = 1; i < M->n_cell; i++)
        if (M->cells[i].type == t) n++;
    return n;
}

/* ---- A combinational cell becomes a LUT carrying its table ---- */

static void nl_comb(void)
{
    static const char *src =
        "module tiny (a, b, y);\n"
        " input a;\n input b;\n output y;\n"
        " sky130_fd_sc_hd__and2_2 u1 (.A(a), .B(b), .X(y));\n"
        "endmodule\n";
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);
    CHECK(nl_cnt(M, RT_LUT) == 1);
    CHECK(cd->n_cell == 1);

    /* The recovered cell must still know it's an AND */
    {
        uint32_t i;
        for (i = 1; i < M->n_cell; i++) {
            if (M->cells[i].type != RT_LUT) continue;
            CHECK(M->cells[i].cdix < cd->n_cell);
            CHECK(cd->cells[M->cells[i].cdix].n_in == 2);
            CHECK(nl_msk(&cd->cells[M->cells[i].cdix]) == 0x8);
            CHECK(M->cells[i].n_in == 2);
        }
    }

    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_comb)

/* ---- A flop stays a flop, with D/CLK/RESET in the right slots ---- */

static void nl_ff(void)
{
    static const char *src =
        "module tiny (clk, d, q, rst_n);\n"
        " input clk;\n input d;\n input rst_n;\n output q;\n"
        " sky130_fd_sc_hd__dfrtp_2 u1 (.CLK(clk), .D(d),"
        " .RESET_B(rst_n), .Q(q));\n"
        "endmodule\n";
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;
    uint32_t i, nd = 0, nc = 0, nr = 0, nq = 0;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);
    CHECK(nl_cnt(M, RT_DFFR) == 1);
    CHECK(nl_cnt(M, RT_DFF) == 0);   /* no phantom second flop */
    CHECK(nl_cnt(M, RT_LUT) == 0);

    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        const char *s = M->strs + n->name_off;
        if (n->name_len == 1 && s[0] == 'd') nd = i;
        if (n->name_len == 1 && s[0] == 'q') nq = i;
        if (n->name_len == 3 && memcmp(s, "clk", 3) == 0) nc = i;
        if (n->name_len == 5 && memcmp(s, "rst_n", 5) == 0) nr = i;
    }
    CHECK(nd && nc && nr && nq);

    for (i = 1; i < M->n_cell; i++) {
        if (M->cells[i].type != RT_DFFR) continue;
        CHECK(M->cells[i].n_in == 3);
        CHECK(M->cells[i].ins[0] == nd);
        CHECK(M->cells[i].ins[1] == nc);
        CHECK(M->cells[i].ins[2] == nr);
        CHECK(M->cells[i].out == nq);
    }

    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_ff)

/* ---- Fill and tap cells have no function and make no cells ---- */

static void nl_phys(void)
{
    static const char *src =
        "module tiny (a, y);\n"
        " input a;\n output y;\n"
        " sky130_fd_sc_hd__decap_3 f1 ();\n"
        " sky130_fd_sc_hd__inv_2 u1 (.A(a), .Y(y));\n"
        "endmodule\n";
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);
    CHECK(nl_cnt(M, RT_LUT) == 1);   /* the inverter, and only it */
    CHECK(cd->n_cell == 1);

    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_phys)

/* ---- A tie cell hands you two outputs from one instance ----
 * conb drives HI and LO. One rt_cell_t has one output, so this
 * has to become two cells reading different columns of the same
 * table, or half the constants in the design quietly vanish. */

static void nl_multi(void)
{
    static const char *src =
        "module tiny (h, l);\n"
        " output h;\n output l;\n"
        " sky130_fd_sc_hd__conb_1 u1 (.HI(h), .LO(l));\n"
        "endmodule\n";
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;
    uint32_t i, nh = 0, nl = 0, seen_hi = 0, seen_lo = 0;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);
    CHECK(nl_cnt(M, RT_LUT) == 2);
    CHECK(cd->n_cell == 1);          /* one table, two readers */

    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        const char *s = M->strs + n->name_off;
        if (n->name_len == 1 && s[0] == 'h') nh = i;
        if (n->name_len == 1 && s[0] == 'l') nl = i;
    }
    CHECK(nh && nl);

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        const cd_cell_t *t;
        if (c->type != RT_LUT) continue;
        CHECK(c->cdix < cd->n_cell);
        t = &cd->cells[c->cdix];
        CHECK(c->param >= 0 && c->param < t->n_out);
        /* HI is constant 1, LO is constant 0 */
        if (c->out == nh) {
            CHECK(t->rows[0].outs[c->param] == 1);
            seen_hi = 1;
        }
        if (c->out == nl) {
            CHECK(t->rows[0].outs[c->param] == 0);
            seen_lo = 1;
        }
    }
    CHECK(seen_hi && seen_lo);

    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_multi)

/* ---- Sequential structure, recovered from connectivity ----
 * Three flops wired Q to D. Nothing in the netlist says "shift
 * register" and we shouldn't need it to. */

static void nl_shift(void)
{
    static const char *src =
        "module tiny (clk, si, q2, rst_n);\n"
        " input clk;\n input si;\n input rst_n;\n output q2;\n"
        " wire q0; wire q1;\n"
        " sky130_fd_sc_hd__dfrtp_2 f0 (.CLK(clk), .D(si),"
        " .RESET_B(rst_n), .Q(q0));\n"
        " sky130_fd_sc_hd__dfrtp_2 f1 (.CLK(clk), .D(q0),"
        " .RESET_B(rst_n), .Q(q1));\n"
        " sky130_fd_sc_hd__dfrtp_2 f2 (.CLK(clk), .D(q1),"
        " .RESET_B(rst_n), .Q(q2));\n"
        "endmodule\n";
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;
    sq_res_t *R;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    R   = (sq_res_t *)calloc(1, sizeof(sq_res_t));
    CHECK(lib != NULL && cd != NULL && R != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);
    CHECK(sq_scan(M, R) == 0);

    CHECK(R->n_ff == 3);
    CHECK(R->n_chain == 1);
    CHECK(R->chlen[0] == 3);

    /* Head is fed by a port, so it holds; the rest shift */
    CHECK(R->ff[R->chhead[0]].kind == SQ_HOLD);
    CHECK(R->ff[R->chhead[0]].pos == 0);

    {
        uint32_t i, nshift = 0;
        for (i = 0; i < R->n_ff; i++)
            if (R->ff[i].kind == SQ_SHIFT) nshift++;
        CHECK(nshift == 2);
    }

    free(R);
    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_shift)

/* ---- Flops fed by many flops are state, not a shift stage ---- */

static void nl_fsm(void)
{
    static const char *src =
        "module tiny (clk, rst_n, q0, q1);\n"
        " input clk;\n input rst_n;\n output q0;\n output q1;\n"
        " wire x;\n"
        " sky130_fd_sc_hd__xor2_2 g0 (.A(q0), .B(q1), .X(x));\n"
        " sky130_fd_sc_hd__dfrtp_2 f0 (.CLK(clk), .D(x),"
        " .RESET_B(rst_n), .Q(q0));\n"
        " sky130_fd_sc_hd__dfrtp_2 f1 (.CLK(clk), .D(q0),"
        " .RESET_B(rst_n), .Q(q1));\n"
        "endmodule\n";
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;
    sq_res_t *R;
    uint32_t i, nfsm = 0;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    R   = (sq_res_t *)calloc(1, sizeof(sq_res_t));
    CHECK(lib != NULL && cd != NULL && R != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);
    CHECK(sq_scan(M, R) == 0);
    CHECK(R->n_ff == 2);

    /* f0's D is q0^q1, so it sees itself plus one other. The self
     * reference is recorded separately, leaving one real source. */
    for (i = 0; i < R->n_ff; i++) {
        if (R->ff[i].self) {
            CHECK(R->ff[i].nsrc == 1);
            nfsm++;
        }
    }
    CHECK(nfsm == 1);

    free(R);
    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_fsm)

/* ---- Run the recovered warm-up and watch success ----
 * 00_source.v asserts S when A + B == 496. Shift two bytes in
 * MSB first, tick eight times, and the recovered netlist should
 * agree. 248 + 248 hits it; 248 + 247 must not. */

static int
nl_try(const rt_mod_t *M, const cd_lib_t *cd, uint32_t a, uint32_t b)
{
    sm_st_t S;
    uint32_t nA, nB, nS, nclk, nen, nrst;
    int i, r = -1;

    if (sm_init(&S, M) != 0) return -1;

    nA = sm_net(M, "A");  nB = sm_net(M, "B");  nS = sm_net(M, "S");
    nclk = sm_net(M, "clk"); nen = sm_net(M, "en");
    nrst = sm_net(M, "rst_n");
    if (!nA || !nB || !nS || !nclk || !nen || !nrst) goto out;

    /* Reset low, settle, then release */
    sm_set(&S, nrst, 0);
    sm_set(&S, nen, 0);
    sm_set(&S, nclk, 0);
    if (sm_eval(M, cd, &S) != 0) goto out;
    sm_set(&S, nrst, 1);
    sm_set(&S, nen, 1);

    for (i = 7; i >= 0; i--) {
        sm_set(&S, nA, (uint8_t)((a >> i) & 1));
        sm_set(&S, nB, (uint8_t)((b >> i) & 1));
        if (sm_tick(M, cd, &S) != 0) goto out;
    }

    r = sm_get(&S, nS);
out:
    sm_free(&S);
    return r;
}

static void nl_sim(void)
{
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;
    char *src;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");
    if (!th_exist(WARMUP)) SKIP("no warm-up netlist");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    src = nl_slurp(WARMUP);
    CHECK(src != NULL);
    M = nl_str(src, lib, cd);
    CHECK(M != NULL);

    printf("  sim: 248+248=%d  248+247=%d  0+0=%d  255+241=%d\n",
           nl_try(M, cd, 248, 248), nl_try(M, cd, 248, 247),
           nl_try(M, cd, 0, 0), nl_try(M, cd, 255, 241));

    CHECK(nl_try(M, cd, 248, 248) == 1);   /* 496 */
    CHECK(nl_try(M, cd, 255, 241) == 1);   /* 496 the other way */
    CHECK(nl_try(M, cd, 248, 247) == 0);   /* 495 */
    CHECK(nl_try(M, cd, 249, 248) == 0);   /* 497 */
    CHECK(nl_try(M, cd, 0, 0) == 0);
    CHECK(nl_try(M, cd, 255, 255) == 0);   /* 510 */

    free(src);
    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_sim)

/* ---- The real thing: Jane Street's warm-up netlist ----
 * 79 logic cells, of which 16 are resettable flops. The 93 tap
 * and 58 decap cells carry no function and must not become
 * anything at all. */

static void nl_warm(void)
{
    lb_lib_t *lib;
    cd_lib_t *cd;
    rt_mod_t *M;
    char *src;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");
    if (!th_exist(WARMUP)) SKIP("no warm-up netlist");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    src = nl_slurp(WARMUP);
    CHECK(src != NULL);

    M = nl_str(src, lib, cd);
    CHECK(M != NULL);

    printf("  warm-up: %u nets, %u cells (%u LUT, %u DFFR)\n",
           M->n_net, M->n_cell - 1,
           nl_cnt(M, RT_LUT), nl_cnt(M, RT_DFFR));

    CHECK(nl_cnt(M, RT_LUT) == 63);
    CHECK(nl_cnt(M, RT_DFFR) == 16);
    CHECK(nl_cnt(M, RT_DFF) == 0);

    /* 19 distinct cell types in the netlist, minus tap, decap and
     * the flop, all of which are interned elsewhere or not at all */
    CHECK(cd->n_cell > 8 && cd->n_cell < 20);

    /* 00_source.v is two 8-bit shift registers feeding an adder
     * and a comparator. Recover that from the wires alone. */
    {
        sq_res_t *R = (sq_res_t *)calloc(1, sizeof(sq_res_t));
        CHECK(R != NULL);
        CHECK(sq_scan(M, R) == 0);
        CHECK(R->n_ff == 16);
        CHECK(R->n_chain == 2);
        CHECK(R->chlen[0] == 8);
        CHECK(R->chlen[1] == 8);
        printf("  warm-up seq: %u chains of %u and %u\n",
               R->n_chain, R->chlen[0], R->chlen[1]);
        free(R);
    }

    free(src);
    rt_free(M); free(M);
    free(cd); free(lib);
    PASS();
}
TH_REG("nlst", nl_warm)
