/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tsat.c -- CNF construction and the CDCL solver
 * An untested SAT solver is an elaborate random number generator. */

#include "tharns.h"
#include "takahe.h"

/* ---- Does a model actually satisfy the formula? ----
 * Cheap to check and the only thing standing between us and a solver
 * that confidently returns nonsense. */

static int
sat_ok(const cn_t *C, const uint8_t *m)
{
    uint32_t i, j;
    for (i = 0; i < C->n_cls; i++) {
        int sat = 0;
        for (j = C->cls[i]; j < C->cls[i + 1]; j++) {
            int32_t l = C->lits[j];
            uint8_t v = m[l > 0 ? l : -l];
            if ((l > 0 && v) || (l < 0 && !v)) { sat = 1; break; }
        }
        if (!sat) return 0;
    }
    return 1;
}

/* ---- Trivial contradiction ---- */

static void sa_unsat(void)
{
    cn_t C;
    uint8_t m[8];
    int32_t a[1];

    CHECK(cn_init(&C, 64, 256) == 0);
    cn_var(&C);
    a[0] = 1;  CHECK(cn_add(&C, a, 1) == 0);
    a[0] = -1; CHECK(cn_add(&C, a, 1) == 0);
    CHECK(sa_solve(&C, m, 10000) == 0);
    cn_free(&C);
    PASS();
}
TH_REG("sat", sa_unsat)

/* ---- Forced assignment ---- */

static void sa_unit(void)
{
    cn_t C;
    uint8_t m[8];
    int32_t cl[2];

    CHECK(cn_init(&C, 64, 256) == 0);
    cn_var(&C); cn_var(&C);
    cl[0] = 1; cl[1] = 2;  CHECK(cn_add(&C, cl, 2) == 0);   /* a | b  */
    cl[0] = -1;            CHECK(cn_add(&C, cl, 1) == 0);   /* !a     */
    CHECK(sa_solve(&C, m, 10000) == 1);
    CHECK(m[1] == 0);
    CHECK(m[2] == 1);
    CHECK(sat_ok(&C, m));
    cn_free(&C);
    PASS();
}
TH_REG("sat", sa_unit)

/* ---- Pigeonhole: four pigeons, three holes, no room ----
 * Small but genuinely needs conflict learning to refute. */

static void sa_php(void)
{
    cn_t C;
    uint8_t m[64];
    int32_t cl[8];
    int p, h, p2;

    CHECK(cn_init(&C, 4096, 65536) == 0);
    for (p = 0; p < 4 * 3; p++) cn_var(&C);

    /* every pigeon is in some hole */
    for (p = 0; p < 4; p++) {
        for (h = 0; h < 3; h++) cl[h] = p * 3 + h + 1;
        CHECK(cn_add(&C, cl, 3) == 0);
    }
    /* no hole takes two pigeons */
    for (h = 0; h < 3; h++)
        for (p = 0; p < 4; p++)
            for (p2 = p + 1; p2 < 4; p2++) {
                cl[0] = -(p * 3 + h + 1);
                cl[1] = -(p2 * 3 + h + 1);
                CHECK(cn_add(&C, cl, 2) == 0);
            }

    CHECK(sa_solve(&C, m, 200000) == 0);
    cn_free(&C);
    PASS();
}
TH_REG("sat", sa_php)

/* ---- Three pigeons, three holes: satisfiable ---- */

static void sa_php3(void)
{
    cn_t C;
    uint8_t m[64];
    int32_t cl[8];
    int p, h, p2;

    CHECK(cn_init(&C, 4096, 65536) == 0);
    for (p = 0; p < 9; p++) cn_var(&C);
    for (p = 0; p < 3; p++) {
        for (h = 0; h < 3; h++) cl[h] = p * 3 + h + 1;
        CHECK(cn_add(&C, cl, 3) == 0);
    }
    for (h = 0; h < 3; h++)
        for (p = 0; p < 3; p++)
            for (p2 = p + 1; p2 < 3; p2++) {
                cl[0] = -(p * 3 + h + 1);
                cl[1] = -(p2 * 3 + h + 1);
                CHECK(cn_add(&C, cl, 2) == 0);
            }
    CHECK(sa_solve(&C, m, 200000) == 1);
    CHECK(sat_ok(&C, m));
    cn_free(&C);
    PASS();
}
TH_REG("sat", sa_php3)

/* ---- Truth-table encoding round-trips ----
 * Build CNF for a real library cell and check the solver agrees with
 * cd_eval on every input combination. */

#define SKY130_LIB "C:/dev/documents/skywater/sky130_fd_sc_hd__tt_025C_1v80.lib"

static const lb_cell_t *
sat_fnd(const lb_lib_t *lib, const char *name)
{
    uint32_t i;
    size_t n = strlen(name);
    for (i = 0; i < lib->n_cell; i++) {
        const lb_cell_t *c = &lib->cells[i];
        if (c->name_len == n &&
            memcmp(lib->strs + c->name_off, name, n) == 0) return c;
    }
    return NULL;
}

static void sa_lut(void)
{
    lb_lib_t *lib;
    cd_cell_t *t;
    const lb_cell_t *c;
    uint32_t combo;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");
    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    t = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && t != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);
    c = sat_fnd(lib, "sky130_fd_sc_hd__a31o_2");
    CHECK(c != NULL);
    CHECK(lb_fbld(lib, c, t) == 0);
    CHECK(t->n_in == 4);

    /* For each input pattern, pin the inputs and ask for the output */
    for (combo = 0; combo < 16u; combo++) {
        cn_t C;
        uint8_t m[16];
        uint32_t ins[4], out;
        int8_t iv[CD_MAX_VALS], ov[CD_MAX_VALS];
        uint8_t j;

        CHECK(cn_init(&C, 256, 4096) == 0);
        for (j = 0; j < 4; j++) ins[j] = cn_var(&C);
        out = cn_var(&C);
        CHECK(cn_lut(&C, t, 0, ins, 4, out) == 0);
        for (j = 0; j < 4; j++) {
            int32_t u = (combo >> j) & 1u ? (int32_t)ins[j]
                                          : -(int32_t)ins[j];
            CHECK(cn_unit(&C, u) == 0);
            iv[j] = (int8_t)((combo >> j) & 1u);
        }
        CHECK(sa_solve(&C, m, 100000) == 1);
        CHECK(cd_eval(t, iv, ov) == 0);
        CHECK(m[out] == (uint8_t)ov[0]);
        cn_free(&C);
    }

    free(t); free(lib);
    PASS();
}
TH_REG("sat", sa_lut)

/* ---- Miter: equivalent designs are unsatisfiable ----
 * Build two little modules by hand, one a XOR built from AND/OR/NOT,
 * the other a plain XOR. Same function, different gates. The miter
 * should find no input that tells them apart. */

static rt_mod_t *
sat_mk(int as_xor)
{
    rt_mod_t *M = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    uint32_t a, b, y, ins[2];

    if (!M) return NULL;
    if (rt_init(M, 64, 64) != 0) { free(M); return NULL; }
    a = rt_anet(M, "a", 1, 1, 1, TK_RADIX_BIN);
    b = rt_anet(M, "b", 1, 1, 1, TK_RADIX_BIN);
    y = rt_anet(M, "y", 1, 1, 2, TK_RADIX_BIN);

    if (as_xor) {
        ins[0] = a; ins[1] = b;
        rt_acell(M, RT_XOR, y, ins, 2, 1);
    } else {
        /* y = (a|b) & !(a&b) */
        uint32_t o = rt_anet(M, "o", 1, 1, 0, TK_RADIX_BIN);
        uint32_t n = rt_anet(M, "n", 1, 1, 0, TK_RADIX_BIN);
        ins[0] = a; ins[1] = b; rt_acell(M, RT_OR,   o, ins, 2, 1);
        ins[0] = a; ins[1] = b; rt_acell(M, RT_NAND, n, ins, 2, 1);
        ins[0] = o; ins[1] = n; rt_acell(M, RT_AND,  y, ins, 2, 1);
    }
    return M;
}

static void sa_mitr_eq(void)
{
    cn_t C;
    rt_mod_t *A = sat_mk(1), *B = sat_mk(0);
    uint8_t *m;

    CHECK(A != NULL && B != NULL);
    CHECK(cn_init(&C, 100000, 800000) == 0);
    CHECK(cn_mitr(&C, A, B, NULL) != 0);

    m = (uint8_t *)calloc(C.n_var + 2, 1);
    CHECK(m != NULL);
    /* UNSAT means nothing distinguishes them, which is the proof */
    CHECK(sa_solve(&C, m, 1000000) == 0);

    free(m); cn_free(&C);
    rt_free(A); free(A); rt_free(B); free(B);
    PASS();
}
TH_REG("sat", sa_mitr_eq)

/* ---- Miter: a real difference must be found ----
 * Same test, except one module computes OR instead of XOR. The solver
 * has to produce the input that separates them, and a miter that says
 * UNSAT here would be proving equivalence that isn't there. */

static void sa_mitr_ne(void)
{
    cn_t C;
    rt_mod_t *A = sat_mk(1), *B;
    uint8_t *m;
    uint32_t a, b, y, ins[2];

    B = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    CHECK(A != NULL && B != NULL);
    CHECK(rt_init(B, 64, 64) == 0);
    a = rt_anet(B, "a", 1, 1, 1, TK_RADIX_BIN);
    b = rt_anet(B, "b", 1, 1, 1, TK_RADIX_BIN);
    y = rt_anet(B, "y", 1, 1, 2, TK_RADIX_BIN);
    ins[0] = a; ins[1] = b;
    rt_acell(B, RT_OR, y, ins, 2, 1);   /* differs when a and b are both 1 */

    CHECK(cn_init(&C, 100000, 800000) == 0);
    CHECK(cn_mitr(&C, A, B, NULL) != 0);
    m = (uint8_t *)calloc(C.n_var + 2, 1);
    CHECK(m != NULL);
    CHECK(sa_solve(&C, m, 1000000) == 1);
    CHECK(sat_ok(&C, m));

    free(m); cn_free(&C);
    rt_free(A); free(A); rt_free(B); free(B);
    PASS();
}
TH_REG("sat", sa_mitr_ne)

/* ---- A conflict on the very first clause ----
 * sa_prop hands back the index of the clause it tripped over and reserves
 * zero for "nothing tripped". Let a real clause sit at index 0 and its
 * conflicts read as no conflict at all, so the search carries on from a
 * trail it has already contradicted and eventually calls an unsatisfiable
 * formula satisfiable. Clause order alone decides whether that bites, so
 * the first clause added here is deliberately the one that fails. */

static void
sa_cls0(void)
{
    cn_t C;
    uint8_t m[16];
    int32_t cl[2];

    CHECK(cn_init(&C, 64, 256) == 0);
    cn_var(&C); cn_var(&C);
    cl[0] = 1; cl[1] = 2;  CHECK(cn_add(&C, cl, 2) == 0);   /* a | b */
    cl[0] = -1;            CHECK(cn_add(&C, cl, 1) == 0);   /* !a    */
    cl[0] = -2;            CHECK(cn_add(&C, cl, 1) == 0);   /* !b    */
    CHECK(sa_solve(&C, m, 100000) == 0);
    cn_free(&C);
    PASS();
}
TH_REG("sat", sa_cls0)

/* ---- Solving the same formula again after tightening it ----
 * The fault injector blocks a fault it has already reported and asks
 * again, so the second answer has to be as sound as the first. */

static void
sa_again(void)
{
    cn_t C;
    uint8_t m[16];
    int32_t cl[3];

    CHECK(cn_init(&C, 64, 256) == 0);
    cn_var(&C); cn_var(&C); cn_var(&C);
    cl[0] = 1; cl[1] = 2; cl[2] = 3;
    CHECK(cn_add(&C, cl, 3) == 0);          /* a | b | c */
    CHECK(sa_solve(&C, m, 100000) == 1);
    CHECK(sat_ok(&C, m));

    cl[0] = -1; CHECK(cn_add(&C, cl, 1) == 0);
    cl[0] = -2; CHECK(cn_add(&C, cl, 1) == 0);
    CHECK(sa_solve(&C, m, 100000) == 1);    /* c must carry the clause */
    CHECK(sat_ok(&C, m));
    CHECK(m[3] == 1);

    cl[0] = -3; CHECK(cn_add(&C, cl, 1) == 0);
    CHECK(sa_solve(&C, m, 100000) == 0);
    cn_free(&C);
    PASS();
}
TH_REG("sat", sa_again)
