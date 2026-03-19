/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tmap.c -- Technology mapping tests
 * Where abstract gates meet silicon reality. */

#include "tharns.h"
#include "takahe.h"

#define SKY130_LIB "C:/dev/documents/skywater/sky130_fd_sc_hd__tt_025C_1v80.lib"

/* ---- Liberty parser test ---- */

static void mp_lib(void)
{
    lb_lib_t *lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    CHECK(lib != NULL);

    if (th_exist(SKY130_LIB)) {
        int rc = lb_load(lib, SKY130_LIB);
        CHECK(rc == 0);
        CHECK(lib->n_cell > 400);  /* SKY130 HD has ~428 */
        CHECK(lib->n_cell <= LB_MAX_CELLS);
    } else {
        SKIP("no sky130 .lib");
    }

    free(lib);
    PASS();
}
TH_REG("map", mp_lib)

/* ---- Cell binding test ---- */

static void mp_bnd(void)
{
    lb_lib_t *lib;
    mp_bind_t tbl[RT_CELL_COUNT];

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    CHECK(lib != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    mp_bind(lib, tbl);

    /* Essential gates must be bound */
    CHECK(tbl[RT_AND].valid);
    CHECK(tbl[RT_OR].valid);
    CHECK(tbl[RT_XOR].valid);
    CHECK(tbl[RT_NOT].valid);
    CHECK(tbl[RT_MUX].valid);
    CHECK(tbl[RT_DFF].valid);
    CHECK(tbl[RT_DFFR].valid);
    CHECK(tbl[RT_CONST].valid);

    free(lib);
    PASS();
}
TH_REG("map", mp_bnd)

/* ---- Bit-blast test: AND width=4 → 4 cells of width 1 ---- */

static void mp_bbw(void)
{
    rt_mod_t M;
    uint32_t na, nb, nc, ins[2];
    int blasted;

    CHECK(rt_init(&M, 1024, 4096) == 0);

    na = rt_anet(&M, "a", 1, 4, 1, TK_RADIX_BIN);
    nb = rt_anet(&M, "b", 1, 4, 1, TK_RADIX_BIN);
    nc = rt_anet(&M, "c", 1, 4, 2, TK_RADIX_BIN);
    ins[0] = na; ins[1] = nb;
    rt_acell(&M, RT_AND, nc, ins, 2, 4);

    blasted = mp_bblst(&M);

    /* Should have created 4 1-bit AND cells */
    CHECK(blasted >= 4);
    /* Original cell should be dead */
    CHECK(M.cells[1].type == RT_CELL_COUNT);

    rt_free(&M);
    PASS();
}
TH_REG("map", mp_bbw)

/* ---- End-to-end CLI test ---- */

static void mp_cli(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");
    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    rc = th_run(TK_BIN " --lib " SKY130_LIB
                " --map tests/out_mapped.v tests/smoke.sv",
                obuf, TH_BUFSZ);
    CHECK(rc == 0);
    CHECK(strstr(obuf, "wrote") != NULL);
    CHECK(strstr(obuf, "emitted") != NULL);
    CHECK(th_exist("tests/out_mapped.v"));

    PASS();
}
TH_REG("map", mp_cli)

/* ---- Cell definition loader: binary ---- */

static void cd_bin(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells.def")) SKIP("no cells.def");
    CHECK(cd_load(lib, "defs/cells.def") == 0);
    CHECK(lib->n_cell > 0);

    /* Find binary AND */
    c = cd_find(lib, "AND", 2);
    CHECK(c != NULL);
    CHECK(c->n_in == 2);
    CHECK(c->n_out == 1);
    CHECK(c->radix == 2);

    /* Evaluate: AND(1, 1) = 1 */
    ins[0] = 1; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* Evaluate: AND(1, 0) = 0 */
    ins[0] = 1; ins[1] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    free(lib);
    PASS();
}
TH_REG("map", cd_bin)

/* ---- Cell definition loader: ternary ---- */

static void cd_ter(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_ter.def")) SKIP("no cells_ter.def");
    CHECK(cd_load(lib, "defs/cells_ter.def") == 0);
    CHECK(lib->n_cell > 0);

    /* Ternary AND: min(A, B) */
    c = cd_find(lib, "AND", 3);
    CHECK(c != NULL);
    CHECK(c->radix == 3);

    /* min(1, -1) = -1 */
    ins[0] = 1; ins[1] = -1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == -1);

    /* min(0, 1) = 0 */
    ins[0] = 0; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* Ternary NOT: neg(A) — negation is free */
    c = cd_find(lib, "NOT", 3);
    CHECK(c != NULL);

    /* neg(1) = -1 */
    ins[0] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == -1);

    /* neg(-1) = 1 */
    ins[0] = -1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* Ternary MUX: 3-way select */
    c = cd_find(lib, "MUX", 3);
    CHECK(c != NULL);
    CHECK(c->n_in == 4);  /* S, D0, D1, D2 */

    /* sel=-1 → D0. D0=1, D1=0, D2=-1 → 1 */
    ins[0] = -1; ins[1] = 1; ins[2] = 0; ins[3] = -1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* sel=1 → D2. D0=1, D1=0, D2=-1 → -1 */
    ins[0] = 1; ins[1] = 1; ins[2] = 0; ins[3] = -1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == -1);

    /* CONSENSUS: agreement detector */
    c = cd_find(lib, "CONSENSUS", 3);
    CHECK(c != NULL);

    /* cons(1, 1) = 1 (agree) */
    ins[0] = 1; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* cons(1, -1) = 0 (disagree → unknown) */
    ins[0] = 1; ins[1] = -1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* FULL ADDER: balanced ternary addition */
    c = cd_find(lib, "FADD", 3);
    CHECK(c != NULL);
    CHECK(c->n_in == 3);
    CHECK(c->n_out == 2);

    /* 1 + 1 + 0 = 2 = (-1, carry 1) in balanced ternary */
    ins[0] = 1; ins[1] = 1; ins[2] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == -1);  /* sum */
    CHECK(outs[1] == 1);   /* carry */

    free(lib);
    PASS();
}
TH_REG("map", cd_ter)

/* ---- Stochastic cell defs ---- */

static void cd_sto(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_stoch.def")) SKIP("no cells_stoch.def");
    CHECK(cd_load(lib, "defs/cells_stoch.def") == 0);
    CHECK(lib->n_cell > 0);

    /* Stochastic AND = multiplication */
    c = cd_find(lib, "AND", 2);
    CHECK(c != NULL);
    CHECK(c->stoch == 1);

    /* Stochastic MUX = scaled addition */
    c = cd_find(lib, "MUX", 2);
    CHECK(c != NULL);
    CHECK(c->stoch == 1);

    free(lib);
    PASS();
}
TH_REG("map", cd_sto)

/* ---- Ternary cprop: fold ternary AND(-1, 0) via truth table ---- */

static void cd_tcp(void)
{
    rt_mod_t M;
    cd_lib_t *cd;
    uint32_t na, nb, nc, ca, cb;
    uint32_t ins[2];

    if (!th_exist("defs/cells_ter.def")) SKIP("no cells_ter.def");

    cd = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(cd != NULL);
    CHECK(cd_load(cd, "defs/cells_ter.def") == 0);

    CHECK(rt_init(&M, 256, 256) == 0);

    /* Create ternary nets (radix=3) */
    na = rt_anet(&M, "a", 1, 1, 0, TK_RADIX_TER);
    nb = rt_anet(&M, "b", 1, 1, 0, TK_RADIX_TER);
    nc = rt_anet(&M, "c", 1, 1, 2, TK_RADIX_TER);  /* output */

    /* CONST(-1) → a */
    ca = rt_acell(&M, RT_CONST, na, NULL, 0, 1);
    CHECK(ca > 0);
    M.cells[ca].param = -1;

    /* CONST(0) → b */
    cb = rt_acell(&M, RT_CONST, nb, NULL, 0, 1);
    CHECK(cb > 0);
    M.cells[cb].param = 0;

    /* AND(a, b) → c */
    ins[0] = na; ins[1] = nb;
    rt_acell(&M, RT_AND, nc, ins, 2, 1);

    /* Run ternary-aware cprop */
    {
        int chg = op_cprop(&M, cd);
        CHECK(chg > 0);
    }

    /* AND cell should be folded to CONST.
     * ternary AND(-1, 0) = min(-1, 0) = -1 */
    {
        uint32_t drv = M.nets[nc].driver;
        CHECK(drv > 0);
        CHECK(M.cells[drv].type == RT_CONST);
        CHECK(M.cells[drv].param == -1);
    }

    rt_free(&M);
    free(cd);
    PASS();
}
TH_REG("map", cd_tcp)

/* ---- Ternary identity: AND(x, 1) = x via truth table ---- */

static void cd_tid(void)
{
    rt_mod_t M;
    cd_lib_t *cd;
    uint32_t nx, nk, nc, ck;
    uint32_t ins[2];

    if (!th_exist("defs/cells_ter.def")) SKIP("no cells_ter.def");

    cd = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    CHECK(cd != NULL);
    CHECK(cd_load(cd, "defs/cells_ter.def") == 0);

    CHECK(rt_init(&M, 256, 256) == 0);

    /* x is a non-constant ternary input */
    nx = rt_anet(&M, "x", 1, 1, 1, TK_RADIX_TER);
    nk = rt_anet(&M, "k", 1, 1, 0, TK_RADIX_TER);
    nc = rt_anet(&M, "c", 1, 1, 2, TK_RADIX_TER);

    /* CONST(1) → k. In ternary AND, 1 is the identity:
     * min(x, 1) = x for all x in {-1, 0, 1} */
    ck = rt_acell(&M, RT_CONST, nk, NULL, 0, 1);
    M.cells[ck].param = 1;

    /* AND(x, k) → c */
    ins[0] = nx; ins[1] = nk;
    rt_acell(&M, RT_AND, nc, ins, 2, 1);

    /* Cprop should detect identity and rebuf to x */
    {
        int chg = op_cprop(&M, cd);
        CHECK(chg > 0);
    }

    /* c should now be driven by ASSIGN(x), not AND */
    {
        uint32_t drv = M.nets[nc].driver;
        CHECK(drv > 0);
        CHECK(M.cells[drv].type == RT_ASSIGN);
        CHECK(M.cells[drv].ins[0] == nx);
    }

    rt_free(&M);
    free(cd);
    PASS();
}
TH_REG("map", cd_tid)

/* ---- Quantum circuit cell defs ---- */

static void cd_qc(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_qc.def")) SKIP("no cells_qc.def");
    CHECK(cd_load(lib, "defs/cells_qc.def") == 0);
    CHECK(lib->n_cell > 0);

    /* Pauli-X: |0⟩ → |1⟩, |1⟩ → |0⟩ */
    c = cd_find(lib, "X", 2);
    CHECK(c != NULL);
    ins[0] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);
    ins[0] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* CNOT: control=1, target=0 → target flips to 1 */
    c = cd_find(lib, "CNOT", 2);
    CHECK(c != NULL);
    CHECK(c->n_in == 2);
    CHECK(c->n_out == 2);
    ins[0] = 1; ins[1] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);  /* control unchanged */
    CHECK(outs[1] == 1);  /* target flipped */

    /* CNOT: control=0, target=1 → target stays 1 */
    ins[0] = 0; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);
    CHECK(outs[1] == 1);

    /* Toffoli: both controls 1 → target flips */
    c = cd_find(lib, "CCNOT", 2);
    CHECK(c != NULL);
    CHECK(c->n_in == 3);
    CHECK(c->n_out == 3);
    ins[0] = 1; ins[1] = 1; ins[2] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[2] == 1);  /* target flipped */

    /* Toffoli: one control 0 → target unchanged */
    ins[0] = 1; ins[1] = 0; ins[2] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[2] == 1);  /* target unchanged */

    /* SWAP: exchange qubits */
    c = cd_find(lib, "SWAP", 2);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);  /* swapped */
    CHECK(outs[1] == 0);

    /* Fredkin: control=1, swap A and B */
    c = cd_find(lib, "CSWAP", 2);
    CHECK(c != NULL);
    ins[0] = 1; ins[1] = 0; ins[2] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);  /* control unchanged */
    CHECK(outs[1] == 1);  /* swapped */
    CHECK(outs[2] == 0);  /* swapped */

    /* Fredkin: control=0, no swap */
    ins[0] = 0; ins[1] = 0; ins[2] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[1] == 0);  /* not swapped */
    CHECK(outs[2] == 1);  /* not swapped */

    free(lib);
    PASS();
}
TH_REG("map", cd_qc)

/* ---- Pattern matching: NOT+AND→NAND ---- */

static void mp_pat(void)
{
    rt_mod_t M;
    uint32_t na, nb, nc, nand_n;
    uint32_t ins[2], ins1[1];

    CHECK(rt_init(&M, 256, 256) == 0);

    na = rt_anet(&M, "a", 1, 1, 1, TK_RADIX_BIN);
    nb = rt_anet(&M, "b", 1, 1, 1, TK_RADIX_BIN);
    nand_n = rt_anet(&M, "ab", 2, 1, 0, TK_RADIX_BIN);
    nc = rt_anet(&M, "c", 1, 1, 2, TK_RADIX_BIN);

    /* AND(a, b) → ab */
    ins[0] = na; ins[1] = nb;
    rt_acell(&M, RT_AND, nand_n, ins, 2, 1);

    /* NOT(ab) → c */
    ins1[0] = nand_n;
    rt_acell(&M, RT_NOT, nc, ins1, 1, 1);

    /* Pattern match should merge to NAND */
    {
        int chg = op_pmatch(&M);
        CHECK(chg > 0);
    }

    /* c should be driven by NAND now */
    {
        uint32_t drv = M.nets[nc].driver;
        CHECK(drv > 0);
        CHECK(M.cells[drv].type == RT_NAND);
        CHECK(M.cells[drv].ins[0] == na);
        CHECK(M.cells[drv].ins[1] == nb);
    }

    rt_free(&M);
    PASS();
}
TH_REG("map", mp_pat)

/* ---- Duodecimal: the Mesopotamians were right ---- */

static void cd_doz(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_doz.def")) SKIP("no cells_doz.def");
    CHECK(cd_load(lib, "defs/cells_doz.def") == 0);
    CHECK(lib->n_cell > 0);

    /* Dozenal AND: min(7, 4) = 4 */
    c = cd_find(lib, "AND", 12);
    CHECK(c != NULL);
    CHECK(c->radix == 12);
    ins[0] = 7; ins[1] = 4;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 4);

    /* Dozenal NOT: complement of 3 = 8 (11-3) */
    c = cd_find(lib, "NOT", 12);
    CHECK(c != NULL);
    ins[0] = 3;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 8);

    /* Dozenal NOT: double complement = identity */
    ins[0] = 7;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 4);  /* 11-7=4 */
    ins[0] = 4;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 7);  /* 11-4=7. Full circle. */

    /* Dozenal HADD: 7 + 8 = 15 = 1 dozen + 3
     * A Sumerian merchant adding sheep. */
    c = cd_find(lib, "HADD", 12);
    CHECK(c != NULL);
    CHECK(c->n_in == 2);
    CHECK(c->n_out == 2);
    ins[0] = 7; ins[1] = 8;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3);   /* sum: 3 sheep */
    CHECK(outs[1] == 1);   /* carry: 1 dozen */

    /* HADD: 5 + 6 = 11, no carry */
    ins[0] = 5; ins[1] = 6;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 11);
    CHECK(outs[1] == 0);

    /* HADD: 11 + 11 = 22 = 1 dozen + 10 */
    ins[0] = 11; ins[1] = 11;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 10);  /* ↊ (dek) */
    CHECK(outs[1] == 1);   /* carry */

    /* CONSENSUS: two scribes agree on 9 */
    c = cd_find(lib, "CONSENSUS", 12);
    CHECK(c != NULL);
    ins[0] = 9; ins[1] = 9;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 9);

    /* CONSENSUS: scribes disagree → 0 */
    ins[0] = 3; ins[1] = 7;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    free(lib);
    PASS();
}
TH_REG("map", cd_doz)

/* ---- DNA computing: Watson-Crick in silicon ---- */

static void cd_dna(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_dna.def")) SKIP("no cells_dna.def");
    CHECK(cd_load(lib, "defs/cells_dna.def") == 0);

    /* Watson-Crick complement: A↔T, G↔C */
    c = cd_find(lib, "NOT", 4);
    CHECK(c != NULL);
    ins[0] = 0; /* A */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1); /* T */
    ins[0] = 2; /* G */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3); /* C */

    /* Double complement = identity (DNA backup) */
    ins[0] = 3; /* C */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 2); /* G */
    ins[0] = 2; /* and back */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3); /* C. Life works. */

    /* MATCH: A-T is Watson-Crick pair → 3 (strong) */
    c = cd_find(lib, "MATCH", 4);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 1; /* A-T */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3);

    /* MATCH: A-G is mismatch → 0 */
    ins[0] = 0; ins[1] = 2;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* CODON: the ribosome gate */
    c = cd_find(lib, "CODON", 4);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 0; ins[2] = 0; /* AAA = codon 0 */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);
    ins[0] = 3; ins[1] = 3; ins[2] = 3; /* CCC = codon 63 */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 63);

    free(lib);
    PASS();
}
TH_REG("map", cd_dna)

/* ---- Epistemic logic: Bochvar's revenge ---- */

static void cd_epi(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_epist.def")) SKIP("no cells_epist.def");
    CHECK(cd_load(lib, "defs/cells_epist.def") == 0);

    /* NOT: T|J (6) → F|J (3). Negate assertion, keep warrant */
    c = cd_find(lib, "NOT", 7);
    CHECK(c != NULL);
    ins[0] = 6;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3);

    /* NOT: Meaningless stays Meaningless */
    ins[0] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* CONSENSUS: T|U (5) + T|J (6) → T|J (6) — corroboration */
    c = cd_find(lib, "CONSENSUS", 7);
    CHECK(c != NULL);
    ins[0] = 5; ins[1] = 6;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 6); /* justified! */

    /* CONSENSUS: T|J (6) vs F|J (3) → 0 (conflict) */
    ins[0] = 6; ins[1] = 3;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0); /* irreconcilable */

    /* DEFEAT: T|J (6) defeated by T|J (6) → T|D (4) */
    c = cd_find(lib, "DEFEAT", 7);
    CHECK(c != NULL);
    ins[0] = 6; ins[1] = 6;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 4); /* warrant revoked */

    /* DEFEAT: T|U (5) not defeated by F|U (2) → T|U (5) unchanged */
    ins[0] = 5; ins[1] = 2;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 5); /* unaffected */

    /* AND: T|J & N = N (infectious nonsense) */
    c = cd_find(lib, "AND", 7);
    CHECK(c != NULL);
    ins[0] = 6; ins[1] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0); /* Bochvar's infection */

    free(lib);
    PASS();
}
TH_REG("map", cd_epi)

/* ---- Cellular automata: Rule 110 is Turing-complete ---- */

static void cd_life(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_life.def")) SKIP("no cells_life.def");
    CHECK(cd_load(lib, "defs/cells_life.def") == 0);

    /* Rule 110: 110 → 1 (the signature pattern) */
    c = cd_find(lib, "RULE110", 2);
    CHECK(c != NULL);
    ins[0] = 1; ins[1] = 1; ins[2] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* Rule 110: 111 → 0 */
    ins[0] = 1; ins[1] = 1; ins[2] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* Rule 110: 000 → 0 */
    ins[0] = 0; ins[1] = 0; ins[2] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* Rule 30: 100 → 1 (chaos from order) */
    c = cd_find(lib, "RULE30", 2);
    CHECK(c != NULL);
    ins[0] = 1; ins[1] = 0; ins[2] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* Game of Life: dead + 3 neighbours → alive (birth) */
    c = cd_find(lib, "LIFE", 2);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 3;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1); /* it's alive! */

    /* Life: alive + 1 neighbour → dead (loneliness) */
    ins[0] = 1; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0); /* RIP */

    /* Life: alive + 2 neighbours → alive (survival) */
    ins[0] = 1; ins[1] = 2;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1); /* hangs on */

    free(lib);
    PASS();
}
TH_REG("map", cd_life)

/* ---- Affective computing: emotions in hardware ---- */

static void cd_feel(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);

    if (!th_exist("defs/cells_affect.def")) SKIP("no cells_affect.def");
    CHECK(cd_load(lib, "defs/cells_affect.def") == 0);

    /* NOT: Joy(1) → Sadness(5) */
    c = cd_find(lib, "NOT", 8);
    CHECK(c != NULL);
    ins[0] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 5);

    /* NOT: Fear(3) → Anger(7) */
    ins[0] = 3;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 7);

    /* BLEND: Joy + Sadness → Neutral (opposites cancel) */
    c = cd_find(lib, "BLEND", 8);
    CHECK(c != NULL);
    ins[0] = 1; ins[1] = 5;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0); /* emotional equilibrium */

    /* BLEND: Joy + Trust → Love (adjacent dyad → Trust) */
    ins[0] = 1; ins[1] = 2;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 2);

    /* BLEND: Joy + Joy → Joy (reinforcement) */
    ins[0] = 1; ins[1] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* VALENCE: Joy → positive(1) */
    c = cd_find(lib, "VALENCE", 8);
    CHECK(c != NULL);
    ins[0] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* VALENCE: Anger → negative(2) */
    ins[0] = 7;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 2);

    free(lib);
    PASS();
}
TH_REG("map", cd_feel)

/* ---- I Ching: Leibniz was right ---- */
static void cd_ching(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);
    if (!th_exist("defs/cells_iching.def")) SKIP("no iching");
    CHECK(cd_load(lib, "defs/cells_iching.def") == 0);

    /* NOT: Earth(0) ↔ Heaven(7) */
    c = cd_find(lib, "NOT", 8);
    CHECK(c != NULL);
    ins[0] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 7);
    ins[0] = 7;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    /* HEXAGRAM: Earth below, Heaven above = hexagram 7 */
    c = cd_find(lib, "HEXAGRAM", 8);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 7; /* kun below, qian above */
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 7);

    free(lib);
    PASS();
}
TH_REG("map", cd_ching)

/* ---- Music: Bach in silicon ---- */
static void cd_music(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);
    if (!th_exist("defs/cells_music.def")) SKIP("no music");
    CHECK(cd_load(lib, "defs/cells_music.def") == 0);

    /* Transpose C(0) up by P5(7) = G(7) */
    c = cd_find(lib, "TRANSPOSE", 12);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 7;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 7); /* G */

    /* Transpose A(9) up by m3(3) = C(0) — wraps mod 12 */
    ins[0] = 9; ins[1] = 3;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0); /* C */

    /* Invert E(4) = Ab(8). 12-4=8 */
    c = cd_find(lib, "INVERT", 12);
    CHECK(c != NULL);
    ins[0] = 4;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 8);

    /* Consonance: P5(7) → 10 (very consonant) */
    c = cd_find(lib, "CONSONANCE", 12);
    CHECK(c != NULL);
    ins[0] = 7;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 10);

    /* Consonance: tritone(6) → 0 (diabolus in musica) */
    ins[0] = 6;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0);

    free(lib);
    PASS();
}
TH_REG("map", cd_music)

/* ---- Quarks: antimatter is NOT ---- */
static void cd_quark(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);
    if (!th_exist("defs/cells_quark.def")) SKIP("no quarks");
    CHECK(cd_load(lib, "defs/cells_quark.def") == 0);

    /* Antiquark of up(0) = down(1) */
    c = cd_find(lib, "NOT", 6);
    CHECK(c != NULL);
    ins[0] = 0;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    /* Antiquark of charm(2) = strange(3) */
    ins[0] = 2;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3);

    /* Proton: u(0) u(0) d(1) → valid baryon */
    c = cd_find(lib, "BARYON", 6);
    CHECK(c != NULL);
    ins[0] = 0; ins[1] = 0; ins[2] = 1;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 1);

    free(lib);
    PASS();
}
TH_REG("map", cd_quark)

/* ---- Arrow: democracy is impossible ---- */
static void cd_arrow(void)
{
    cd_lib_t *lib = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
    const cd_cell_t *c;
    int8_t ins[4], outs[4];
    CHECK(lib != NULL);
    if (!th_exist("defs/cells_arrow.def")) SKIP("no arrow");
    CHECK(cd_load(lib, "defs/cells_arrow.def") == 0);

    /* DICTATOR: voter 1 always wins regardless of voter 2 */
    c = cd_find(lib, "DICTATOR", 6);
    CHECK(c != NULL);
    ins[0] = 3; ins[1] = 5;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 3); /* voter 1 wins. always. */

    /* CONSENSUS: agreement → that preference */
    c = cd_find(lib, "CONSENSUS", 6);
    CHECK(c != NULL);
    ins[0] = 4; ins[1] = 4;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 4); /* they agree */

    /* CONSENSUS: disagreement → default (0) */
    ins[0] = 2; ins[1] = 5;
    CHECK(cd_eval(c, ins, outs) == 0);
    CHECK(outs[0] == 0); /* deadlock */

    /* The FAIR cell does not exist. QED. */

    free(lib);
    PASS();
}
TH_REG("map", cd_arrow)

/* ---- PCHIP: Fritsch & Carlson in femtoseconds ---- */

static void pc_test(void)
{
    /* Linear data: PCHIP should reproduce exactly */
    int64_t x[] = { 0, 1000, 2000, 3000, 4000 }; /* fs */
    int64_t f[] = { 0, 1000, 2000, 3000, 4000 }; /* fs */
    int64_t r;

    r = pc_lkup(x, f, 5, 500);
    /* Linear interp of linear data → exact 500 */
    CHECK(r >= 490 && r <= 510);  /* within 2% */

    r = pc_lkup(x, f, 5, 2500);
    CHECK(r >= 2450 && r <= 2550);

    /* Monotone data: delay increases with load */
    {
        /* Mimics a real NLDM row: delay vs load */
        int64_t load[] = { 500, 1300, 3400, 8900, 23200 }; /* aF */
        int64_t dly[]  = { TK_NS2FS(0.079), TK_NS2FS(0.086),
                           TK_NS2FS(0.103), TK_NS2FS(0.140),
                           TK_NS2FS(0.235) };
        int64_t d0, d1, d2;

        /* Endpoints should be exact */
        d0 = pc_lkup(load, dly, 5, 500);
        CHECK(d0 == TK_NS2FS(0.079));

        d1 = pc_lkup(load, dly, 5, 23200);
        CHECK(d1 == TK_NS2FS(0.235));

        /* Midpoint should be between neighbours (monotone) */
        d2 = pc_lkup(load, dly, 5, 5000);
        CHECK(d2 > TK_NS2FS(0.086));  /* > f[1] */
        CHECK(d2 < TK_NS2FS(0.140));  /* < f[3] */
    }

    /* 2D lookup: mini NLDM table */
    {
        int64_t slew[] = { TK_NS2FS(0.01), TK_NS2FS(0.05),
                           TK_NS2FS(0.12) };
        int64_t load[] = { TK_PF2AF(0.0005), TK_PF2AF(0.003),
                           TK_PF2AF(0.009) };
        int64_t tab[] = {
            TK_NS2FS(0.079), TK_NS2FS(0.103), TK_NS2FS(0.140),
            TK_NS2FS(0.094), TK_NS2FS(0.117), TK_NS2FS(0.155),
            TK_NS2FS(0.118), TK_NS2FS(0.141), TK_NS2FS(0.179)
        };
        int64_t r2;

        /* Corner: exact match */
        r2 = pc_lk2d(slew, 3, load, 3, tab,
                      TK_NS2FS(0.01), TK_PF2AF(0.0005));
        CHECK(r2 == TK_NS2FS(0.079));

        /* Interior: should be between surrounding values */
        r2 = pc_lk2d(slew, 3, load, 3, tab,
                      TK_NS2FS(0.03), TK_PF2AF(0.002));
        CHECK(r2 > TK_NS2FS(0.079));  /* > corner min */
        CHECK(r2 < TK_NS2FS(0.179));  /* < corner max */
    }

    PASS();
}
TH_REG("map", pc_test)

/* ---- Espresso: that's that me espresso ---- */

static void es_test(void)
{
    /* XOR function: 2 inputs, 4 minterms.
     * ON-set: {01, 10}. OFF-set: {00, 11}.
     * Espresso should produce 2 product terms:
     *   a'b + ab'  (can't do better for XOR) */
    uint32_t onm[]  = { 1, 2 };     /* 01, 10 */
    uint32_t offm[] = { 0, 3 };     /* 00, 11 */
    char cover[4096];  /* opaque cover storage */
    int nterms;

    nterms = es_mini(onm, 2, offm, 2, 2, cover);
    CHECK(nterms > 0);
    CHECK(nterms <= 2);  /* XOR needs exactly 2 terms */

    /* AND function: 2 inputs.
     * ON-set: {11}. OFF-set: {00, 01, 10}.
     * Should minimise to 1 product term: ab */
    {
        uint32_t on2[]  = { 3 };
        uint32_t off2[] = { 0, 1, 2 };
        nterms = es_mini(on2, 1, off2, 3, 2, cover);
        CHECK(nterms == 1);
    }

    /* OR function: 2 inputs.
     * ON-set: {01, 10, 11}. OFF-set: {00}.
     * Should minimise to 2 terms: a + b */
    {
        uint32_t on3[]  = { 1, 2, 3 };
        uint32_t off3[] = { 0 };
        nterms = es_mini(on3, 3, off3, 1, 2, cover);
        CHECK(nterms <= 2);
    }

    /* 3-input majority: output 1 when 2+ inputs are 1.
     * ON-set: {011,101,110,111} = {3,5,6,7}
     * OFF-set: {000,001,010,100} = {0,1,2,4}
     * Optimal: ab + ac + bc = 3 terms */
    {
        uint32_t on4[]  = { 3, 5, 6, 7 };
        uint32_t off4[] = { 0, 1, 2, 4 };
        nterms = es_mini(on4, 4, off4, 4, 3, cover);
        CHECK(nterms > 0);
        CHECK(nterms <= 4); /* optimal is 3, allow 4 */
    }

    PASS();
}
TH_REG("map", es_test)
