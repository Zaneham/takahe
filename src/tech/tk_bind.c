/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_bind.c -- cell binding
 *
 * Maps each RTL cell type to the best-matching library gate, smallest area
 * wins. The table itself is a lookup, RT_AND to cell index 42.
 *
 * The hard part is working out what a library cell actually does. Substring
 * matching on the function string assumed Liberty was normalised enough to
 * get away with. It isn't, and SKY130 writes xor2 the long way round. So the
 * expression gets parsed into a truth table and the tables get compared,
 * because boolean algebra doesn't care how you spell it.
 */

#include "takahe.h"

/* ---- Classification by truth table ----
 * Substring matching on the function string worked for and2 and quietly
 * gave up on everything harder. The expression gets parsed and evaluated
 * (tk_lfn.c) instead, and the resulting truth table compared against what
 * each gate should be. Costs a few hundred microseconds once, and
 * SKY130's habit of writing xor2 longhand stops mattering. */

#define M_NOT   0x1u   /* 1 input:  f(0)=1               */
#define M_BUF   0x2u   /* 1 input:  f(1)=1               */
#define M_AND   0x8u   /* 2 inputs, bit i = f(i)         */
#define M_OR    0xEu
#define M_XOR   0x6u
#define M_NAND  0x7u
#define M_NOR   0x1u
#define M_XNOR  0x9u

/* Pack the single output column into a bitmask, row index first
 * input in the low bit. Only meaningful for narrow cells. */

static uint32_t
fn_mask(const cd_cell_t *c)
{
    uint32_t m = 0;
    uint16_t r;

    for (r = 0; r < c->n_row && r < 32; r++)
        if (c->rows[r].outs[0]) m |= 1u << r;
    return m;
}

/* A mux is a mux whichever order the pins came in, so try each
 * candidate select line rather than trusting a pin called S. */

static int
fn_mux(const cd_cell_t *c)
{
    uint8_t s, a, b;
    uint16_t r;

    if (c->n_in != 3) return 0;

    for (s = 0; s < 3; s++) {
        for (a = 0; a < 3; a++) {
            if (a == s) continue;
            b = (uint8_t)(3 - s - a);
            for (r = 0; r < c->n_row; r++) {
                const cd_row_t *w = &c->rows[r];
                int8_t want = w->ins[s] ? w->ins[b] : w->ins[a];
                if (w->outs[0] != want) break;
            }
            if (r == c->n_row) return 1;
        }
    }
    return 0;
}

static rt_ctype_t
fn_cls(const lb_lib_t *lib, const lb_cell_t *cell, cd_cell_t *scr)
{
    uint32_t m;

    if (lb_fbld(lib, cell, scr) != 0) return RT_CELL_COUNT;
    if (scr->n_out != 1) return RT_CELL_COUNT;

    m = fn_mask(scr);

    switch ((int)scr->n_in) {
    case 1:
        if (m == M_NOT) return RT_NOT;
        if (m == M_BUF) return RT_BUF;
        return RT_CELL_COUNT;
    case 2:
        if (m == M_AND)  return RT_AND;
        if (m == M_OR)   return RT_OR;
        if (m == M_XOR)  return RT_XOR;
        if (m == M_NAND) return RT_NAND;
        if (m == M_NOR)  return RT_NOR;
        if (m == M_XNOR) return RT_XNOR;
        return RT_CELL_COUNT;
    case 3:
        return fn_mux(scr) ? RT_MUX : RT_CELL_COUNT;
    default:
        return RT_CELL_COUNT;
    }
}

/* ---- Public: bind library cells to RTL types ---- */

int
mp_bind(const lb_lib_t *lib, mp_bind_t *tbl)
{
    uint32_t i;
    int bound = 0;
    /* One scratch table reused across the library. 4KB on the
     * stack rather than a malloc that would need justifying. */
    cd_cell_t scr;

    memset(tbl, 0, (size_t)RT_CELL_COUNT * sizeof(mp_bind_t));

    for (i = 0; i < lib->n_cell; i++) {
        const lb_cell_t *cell = &lib->cells[i];
        rt_ctype_t ct;

        if (cell->special) continue;

        if (cell->kind == LB_DFF) {
            /* RT_DFF is posedge by definition. dfrtn_1 is a whisker
             * smaller than dfrtp_2, so area alone would happily pick
             * the falling-edge part and invert the design.
             *
             * I sat reading Liberty's ff group semantics for far
             * longer than I'd like to admit before the penny
             * dropped. clocked_on is an expression rather than a pin,
             * and a leading ! is the entire difference between rising
             * and falling. One character, whole design. */
            if (cell->negclk) continue;
            ct = cell->rst_pin != 0xFF ? RT_DFFR : RT_DFF;
        } else if (cell->kind == LB_DLAT) {
            ct = RT_DLAT;
        } else if (cell->kind == LB_TIE) {
            ct = RT_CONST;
        } else {
            ct = fn_cls(lib, cell, &scr);
        }

        if (ct >= RT_CELL_COUNT) continue;

        if (!tbl[ct].valid || cell->area < lib->cells[tbl[ct].cell_idx].area) {
            tbl[ct].cell_idx = i;
            tbl[ct].valid = 1;
            bound++;
        }
    }

    if (!tbl[RT_ASSIGN].valid && tbl[RT_BUF].valid)
        tbl[RT_ASSIGN] = tbl[RT_BUF];

    printf("takahe: bound %d RTL types to library cells\n", bound);
    return bound;
}
