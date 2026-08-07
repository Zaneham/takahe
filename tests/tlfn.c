/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tlfn.c -- Liberty function expression tests
 * Checking we understand a31o rather than merely recognising it. */

#include "tharns.h"
#include "takahe.h"
#include <string.h>

#define SKY130_LIB "C:/dev/documents/skywater/sky130_fd_sc_hd__tt_025C_1v80.lib"

/* ---- Find a library cell by name ---- */

static const lb_cell_t *
lf_fnd(const lb_lib_t *lib, const char *name)
{
    uint32_t i;
    size_t n = strlen(name);

    for (i = 0; i < lib->n_cell; i++) {
        const lb_cell_t *c = &lib->cells[i];
        if (c->name_len != n) continue;
        if (memcmp(lib->strs + c->name_off, name, n) == 0) return c;
    }
    return NULL;
}

/* ---- Pin ordinal by name, so tests don't depend on pin order ---- */

static int
lf_pin(const cd_cell_t *c, const char *name, uint8_t dir)
{
    uint8_t j;
    for (j = 0; j < c->n_pin; j++)
        if (c->pdir[j] == dir && strcmp(c->pins[j], name) == 0)
            return dir == 1 ? (int)j : (int)(j - c->n_in);
    return -1;
}

/* ---- Output column as a bitmask, row index low bit first ---- */

static uint32_t
lf_msk(const cd_cell_t *c)
{
    uint32_t m = 0;
    uint16_t r;

    for (r = 0; r < c->n_row && r < 32; r++)
        if (c->rows[r].outs[0]) m |= 1u << r;
    return m;
}

/* ---- Evaluate one output for a named input assignment ---- */

static int
lf_ask(const cd_cell_t *c, const char *const *pins, const int8_t *vals,
       uint8_t n, const char *oname)
{
    int8_t ins[CD_MAX_VALS], outs[CD_MAX_VALS];
    uint8_t k;
    int oi = lf_pin(c, oname, 2);

    memset(ins, 0, sizeof(ins));
    if (oi < 0) return -1;

    for (k = 0; k < n; k++) {
        int pi = lf_pin(c, pins[k], 1);
        if (pi < 0) return -1;
        ins[pi] = vals[k];
    }
    if (cd_eval(c, ins, outs) != 0) return -1;
    return (int)outs[oi];
}

/* ---- Coverage across the whole library ----
 * fn_cls manages 12 of the 69 cell types this design uses.
 * A parser should do considerably better than that. */

static void lf_all(void)
{
    lb_lib_t *lib;
    cd_cell_t *cd;
    uint32_t i, ok = 0, no = 0;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    for (i = 0; i < lib->n_cell; i++) {
        if (lb_fbld(lib, &lib->cells[i], cd) == 0) ok++;
        else no++;
    }

    printf("  lb_fbld: %u built, %u rejected of %u cells\n",
           ok, no, lib->n_cell);
    CHECK(ok > 300);

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_all)

/* ---- Semantics of a complex AOI cell ----
 * a31o: X = (A1&A2&A3) | B1. The exact shape fn_cls cannot see. */

static void lf_a31o(void)
{
    static const char *const p[4] = { "A1", "A2", "A3", "B1" };
    lb_lib_t *lib;
    cd_cell_t *cd;
    const lb_cell_t *c;
    int8_t v[4];

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    c = lf_fnd(lib, "sky130_fd_sc_hd__a31o_2");
    CHECK(c != NULL);
    CHECK(lb_fbld(lib, c, cd) == 0);
    CHECK(cd->n_in == 4);
    CHECK(cd->n_row == 16);

    v[0] = 1; v[1] = 1; v[2] = 1; v[3] = 0;   /* AND arm carries it */
    CHECK(lf_ask(cd, p, v, 4, "X") == 1);

    v[0] = 1; v[1] = 1; v[2] = 0; v[3] = 0;   /* one leg low, so 0 */
    CHECK(lf_ask(cd, p, v, 4, "X") == 0);

    v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 1;   /* B1 alone is enough */
    CHECK(lf_ask(cd, p, v, 4, "X") == 1);

    v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 0;
    CHECK(lf_ask(cd, p, v, 4, "X") == 0);

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_a31o)

/* ---- XOR written as sum-of-products ----
 * SKY130 spells xor2 as (A&!B)|(!A&B), which is why pattern
 * matching on "A^B" was never going to be enough. */

static void lf_xor(void)
{
    static const char *const p[2] = { "A", "B" };
    lb_lib_t *lib;
    cd_cell_t *cd;
    const lb_cell_t *c;
    int8_t v[2];
    int k;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    c = lf_fnd(lib, "sky130_fd_sc_hd__xor2_2");
    CHECK(c != NULL);
    CHECK(lb_fbld(lib, c, cd) == 0);

    for (k = 0; k < 4; k++) {
        v[0] = (int8_t)(k & 1);
        v[1] = (int8_t)((k >> 1) & 1);
        CHECK(lf_ask(cd, p, v, 2, "X") == (v[0] ^ v[1]));
    }

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_xor)

/* ---- Five-input cell with a shared term ----
 * o221a: X = (A1|A2) & (B1|B2) & C1, written out longhand. */

static void lf_o221a(void)
{
    static const char *const p[5] = { "A1", "A2", "B1", "B2", "C1" };
    lb_lib_t *lib;
    cd_cell_t *cd;
    const lb_cell_t *c;
    int8_t v[5];
    int k;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    c = lf_fnd(lib, "sky130_fd_sc_hd__o221a_2");
    CHECK(c != NULL);
    CHECK(lb_fbld(lib, c, cd) == 0);
    CHECK(cd->n_in == 5);
    CHECK(cd->n_row == 32);

    for (k = 0; k < 32; k++) {
        int want;
        v[0] = (int8_t)(k & 1);
        v[1] = (int8_t)((k >> 1) & 1);
        v[2] = (int8_t)((k >> 2) & 1);
        v[3] = (int8_t)((k >> 3) & 1);
        v[4] = (int8_t)((k >> 4) & 1);
        want = (v[0] || v[1]) && (v[2] || v[3]) && v[4];
        CHECK(lf_ask(cd, p, v, 5, "X") == want);
    }

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_o221a)

/* ---- Constant functions and two outputs at once ----
 * conb is the tie cell: HI = 1, LO = 0, no inputs at all.
 * One row, two columns, and a nice check that a truth table
 * over zero variables doesn't fall over. */

static void lf_conb(void)
{
    lb_lib_t *lib;
    cd_cell_t *cd;
    const lb_cell_t *c;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    c = lf_fnd(lib, "sky130_fd_sc_hd__conb_1");
    CHECK(c != NULL);
    CHECK(lb_fbld(lib, c, cd) == 0);
    CHECK(cd->n_in == 0);
    CHECK(cd->n_out == 2);
    CHECK(cd->n_row == 1);
    CHECK(lf_ask(cd, NULL, NULL, 0, "HI") == 1);
    CHECK(lf_ask(cd, NULL, NULL, 0, "LO") == 0);

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_conb)

/* ---- Special-purpose cells stay out of the binding table ----
 * lpflow_inputiso1p is an isolation cell whose function is
 * (A|SLEEP), so a truth table alone says "that's an OR". It is
 * not an OR, it's a power-domain fire door, and binding RT_OR to
 * it once produced a perfectly valid netlist full of them. */

static void lf_spcl(void)
{
    lb_lib_t *lib;
    cd_cell_t *cd;
    const lb_cell_t *iso;
    mp_bind_t tbl[RT_CELL_COUNT];
    int t;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    iso = lf_fnd(lib, "sky130_fd_sc_hd__lpflow_inputiso1p_1");
    CHECK(iso != NULL);
    CHECK(iso->special == 1);

    /* It really does compute OR, which is exactly the trap */
    CHECK(lb_fbld(lib, iso, cd) == 0);
    CHECK(cd->n_in == 2);
    CHECK(lf_msk(cd) == 0xE);

    CHECK(mp_bind(lib, tbl) > 0);
    for (t = 0; t < RT_CELL_COUNT; t++)
        if (tbl[t].valid)
            CHECK(lib->cells[tbl[t].cell_idx].special == 0);

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_spcl)

/* ---- Never bind a posedge flop to a falling-edge cell ----
 * dfrtn_1 is 25.02um2, dfrtp_2 is 26.28um2, so picking purely on
 * area lands you on the negative-edge part. RT_DFF means posedge,
 * and the resulting netlist is wrong in a way that still
 * simulates happily right up until it doesn't. */

static void lf_edge(void)
{
    lb_lib_t *lib;
    mp_bind_t tbl[RT_CELL_COUNT];
    const lb_cell_t *n, *p;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    CHECK(lib != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    /* The loader must actually notice the edge */
    n = lf_fnd(lib, "sky130_fd_sc_hd__dfrtn_1");
    p = lf_fnd(lib, "sky130_fd_sc_hd__dfrtp_2");
    CHECK(n != NULL && p != NULL);
    CHECK(n->negclk == 1);
    CHECK(p->negclk == 0);
    CHECK(n->area < p->area);   /* which is why area alone loses */

    CHECK(mp_bind(lib, tbl) > 0);
    CHECK(tbl[RT_DFF].valid);
    CHECK(tbl[RT_DFFR].valid);
    CHECK(lib->cells[tbl[RT_DFF].cell_idx].negclk == 0);
    CHECK(lib->cells[tbl[RT_DFFR].cell_idx].negclk == 0);

    free(lib);
    PASS();
}
TH_REG("lfn", lf_edge)

/* ---- Sequential cells must be refused, not guessed ----
 * dfrtp says "Q = IQ", where IQ is internal state. A truth
 * table over it would be a confident lie. */

static void lf_seq(void)
{
    lb_lib_t *lib;
    cd_cell_t *cd;
    const lb_cell_t *c;

    if (!th_exist(SKY130_LIB)) SKIP("no sky130 .lib");

    lib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
    cd  = (cd_cell_t *)calloc(1, sizeof(cd_cell_t));
    CHECK(lib != NULL && cd != NULL);
    CHECK(lb_load(lib, SKY130_LIB) == 0);

    c = lf_fnd(lib, "sky130_fd_sc_hd__dfrtp_2");
    CHECK(c != NULL);
    CHECK(lb_fbld(lib, c, cd) == -1);

    free(cd);
    free(lib);
    PASS();
}
TH_REG("lfn", lf_seq)
