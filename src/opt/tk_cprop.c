/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_cprop.c -- Constant propagation for Takahe RTL
 *
 * The algebraic janitorial service, now radix-aware.
 *
 * Binary designs use the fast hardcoded rules (AND(0,x)=0, etc.)
 * that have worked since Tier 3. Ternary and stochastic designs
 * use the universal path: look up the cell's truth table from
 * the .def file and evaluate it. Same result, different path.
 *
 * When all inputs are constant, the truth table gives us the
 * answer directly. When one input is constant, we scan the
 * truth table for identity elements (x op K = x for all x)
 * and annihilators (x op K = C for all x). This is how the
 * engine discovers that ternary AND(-1, x) = -1 without
 * anyone hardcoding it.
 *
 * Brusentsov didn't need an optimiser — the Setun's balanced
 * ternary made everything elegant by construction. We're not
 * that lucky, so we optimise after the fact.
 */

#include "takahe.h"

/* ---- Is net driven by a CONST cell? ---- */

static int
is_cnst(const rt_mod_t *M, uint32_t ni, int64_t *val)
{
    uint32_t drv;
    if (ni == 0 || ni >= M->n_net) return 0;
    drv = M->nets[ni].driver;
    if (drv == 0 || drv >= M->n_cell) return 0;
    if (M->cells[drv].type != RT_CONST) return 0;
    if (val) *val = M->cells[drv].param;
    return 1;
}

/* ---- Fold a cell into a CONST ---- */

static void
fold(rt_mod_t *M, uint32_t ci, int64_t val)
{
    rt_cell_t *c = &M->cells[ci];
    c->type  = RT_CONST;
    c->n_in  = 0;
    c->param = val;
    memset(c->ins, 0, sizeof(c->ins));
}

/* ---- Rewire a cell to BUF (pass-through) ---- */

static void
rebuf(rt_cell_t *c, uint32_t src)
{
    c->type   = RT_ASSIGN;
    c->n_in   = 1;
    c->ins[0] = src;
    memset(c->ins + 1, 0, sizeof(c->ins) - sizeof(c->ins[0]));
}

/* ---- Is value a power of two? Returns log2 or -1 ---- */

static int
ispow2(int64_t v)
{
    int k;
    if (v <= 0) return -1;
    for (k = 0; k < 63; k++) {
        if (v == ((int64_t)1 << k)) return k;
    }
    return -1;
}

/* ---- Universal fold: all inputs const, evaluate truth table ----
 * Works for any radix. Returns 1 if folded, 0 otherwise. */

static int
cd_fold(rt_mod_t *M, uint32_t ci, const cd_lib_t *cd)
{
    rt_cell_t *c = &M->cells[ci];
    const cd_cell_t *def;
    uint8_t radix, j;
    int8_t ins[CD_MAX_VALS], outs[CD_MAX_VALS];
    int64_t vals[RT_MAX_PIN];
    int all_c;

    if (!cd || c->n_in == 0) return 0;

    /* Get radix from output net */
    radix = TK_RADIX_BIN;
    if (c->out > 0 && c->out < M->n_net)
        radix = M->nets[c->out].radix;

    /* Check all inputs are constant */
    all_c = 1;
    for (j = 0; j < c->n_in; j++) {
        if (!is_cnst(M, c->ins[j], &vals[j])) {
            all_c = 0;
            break;
        }
    }
    if (!all_c) return 0;

    /* Find cell definition */
    def = cd_find(cd, rt_cname(c->type), radix);
    if (!def) return 0;

    /* Convert int64_t constants to int8_t for truth table lookup */
    for (j = 0; j < c->n_in && j < def->n_in; j++)
        ins[j] = (int8_t)vals[j];

    if (cd_eval(def, ins, outs) == 0) {
        fold(M, ci, (int64_t)outs[0]);
        return 1;
    }

    return 0;
}

/* ---- Universal identity/annihilator scan ----
 * For a 2-input cell with one const input, check if that
 * constant is an identity (x op K = x) or annihilator
 * (x op K = C for all x). Derived from the truth table.
 * Returns: 1=identity (rebuf to other input),
 *          2=annihilator (fold to constant), 0=neither. */

static int
cd_iden(const cd_lib_t *cd, rt_ctype_t type, uint8_t radix,
        int8_t kval, int kpos, uint32_t *other, int8_t *aval)
{
    const cd_cell_t *def;
    uint16_t r;
    int is_id = 1, is_an = 1;
    int8_t first_out = 0;
    int have_first = 0;

    if (!cd) return 0;
    def = cd_find(cd, rt_cname(type), radix);
    if (!def || def->n_in != 2) return 0;

    for (r = 0; r < def->n_row; r++) {
        const cd_row_t *row = &def->rows[r];
        int8_t kin = (kpos == 0) ? row->ins[0] : row->ins[1];
        int8_t xin = (kpos == 0) ? row->ins[1] : row->ins[0];

        if (kin != kval) continue;

        /* Identity check: output == x (the non-constant input) */
        if (row->outs[0] != xin) is_id = 0;

        /* Annihilator check: output is same constant for all x */
        if (!have_first) {
            first_out = row->outs[0];
            have_first = 1;
        } else if (row->outs[0] != first_out) {
            is_an = 0;
        }
    }

    if (is_id) return 1;
    if (is_an && have_first) {
        *aval = first_out;
        return 2;
    }
    return 0;
}

/* ---- Public: constant propagation pass ---- */

int
op_cprop(rt_mod_t *M, const cd_lib_t *cd)
{
    uint32_t i;
    int chg = 0;
    int64_t v0, v1;

    if (!M) return 0;

    for (i = 1; i < M->n_cell; i++) {
        rt_cell_t *c = &M->cells[i];
        uint8_t radix;
        if (c->type == RT_CELL_COUNT) continue;

        /* Get net radix */
        radix = TK_RADIX_BIN;
        if (c->out > 0 && c->out < M->n_net)
            radix = M->nets[c->out].radix;

        /* ---- Universal path: all-const fold via truth table ----
         * Fires for ANY radix when cell defs are available.
         * Handles ternary AND(-1, 0) = -1, stochastic, etc. */
        if (cd && c->n_in >= 1 && c->n_in <= 4 &&
            c->type != RT_CONST && c->type != RT_ASSIGN) {
            if (cd_fold(M, i, cd)) { chg++; continue; }
        }

        /* ---- Universal identity/annihilator for 2-input cells ----
         * "If one input is K and the truth table says output is
         *  always the other input, K is the identity — rebuf."
         * Works for binary AND(x,1)=x AND ternary OR(x,-1)=x
         * without hardcoding either. */
        if (cd && c->n_in == 2 && radix != TK_RADIX_BIN) {
            int pos;
            for (pos = 0; pos < 2; pos++) {
                int64_t kv;
                if (is_cnst(M, c->ins[pos], &kv)) {
                    int8_t aval = 0;
                    uint32_t otr = c->ins[1 - pos];
                    int res = cd_iden(cd, c->type, radix,
                                     (int8_t)kv, pos, &otr, &aval);
                    if (res == 1) {
                        rebuf(c, c->ins[1 - pos]);
                        chg++; goto next;
                    } else if (res == 2) {
                        fold(M, i, (int64_t)aval);
                        chg++; goto next;
                    }
                }
            }
        }

        /* ---- Binary fast path: hardcoded rules ----
         * These are the original Tier 3 rules. Kept for speed
         * on binary designs — no truth table lookup needed.
         * Only fire for radix=2 nets. */
        if (radix != TK_RADIX_BIN) continue;

        switch ((int)c->type) {
        case RT_ASSIGN:
            if (c->n_in == 1 && is_cnst(M, c->ins[0], &v0)) {
                fold(M, i, v0); chg++;
            }
            break;

        case RT_NOT:
            if (c->n_in == 1 && is_cnst(M, c->ins[0], &v0)) {
                fold(M, i, ~v0); chg++;
            }
            break;

        case RT_AND:
            if (c->n_in == 2) {
                if (is_cnst(M, c->ins[0], &v0) && v0 == 0) {
                    fold(M, i, 0); chg++;
                } else if (is_cnst(M, c->ins[1], &v1) && v1 == 0) {
                    fold(M, i, 0); chg++;
                } else if (is_cnst(M, c->ins[0], &v0) &&
                           is_cnst(M, c->ins[1], &v1)) {
                    fold(M, i, v0 & v1); chg++;
                }
            }
            break;

        case RT_OR:
            if (c->n_in == 2 &&
                is_cnst(M, c->ins[0], &v0) &&
                is_cnst(M, c->ins[1], &v1)) {
                fold(M, i, v0 | v1); chg++;
            }
            break;

        case RT_XOR:
            if (c->n_in == 2 &&
                is_cnst(M, c->ins[0], &v0) &&
                is_cnst(M, c->ins[1], &v1)) {
                fold(M, i, v0 ^ v1); chg++;
            }
            break;

        case RT_ADD:
            if (c->n_in == 2) {
                if (is_cnst(M, c->ins[1], &v1) && v1 == 0) {
                    rebuf(c, c->ins[0]); chg++;
                } else if (is_cnst(M, c->ins[0], &v0) && v0 == 0) {
                    rebuf(c, c->ins[1]); chg++;
                } else if (is_cnst(M, c->ins[0], &v0) &&
                           is_cnst(M, c->ins[1], &v1)) {
                    fold(M, i, v0 + v1); chg++;
                }
            }
            break;

        case RT_SUB:
            if (c->n_in == 2) {
                if (is_cnst(M, c->ins[1], &v1) && v1 == 0) {
                    rebuf(c, c->ins[0]); chg++;
                } else if (is_cnst(M, c->ins[0], &v0) &&
                           is_cnst(M, c->ins[1], &v1)) {
                    fold(M, i, v0 - v1); chg++;
                }
            }
            break;

        case RT_MUL:
            if (c->n_in == 2) {
                int k;
                if (is_cnst(M, c->ins[0], &v0) && v0 == 0) {
                    fold(M, i, 0); chg++;
                } else if (is_cnst(M, c->ins[1], &v1) && v1 == 0) {
                    fold(M, i, 0); chg++;
                } else if (is_cnst(M, c->ins[1], &v1) && v1 == 1) {
                    rebuf(c, c->ins[0]); chg++;
                } else if (is_cnst(M, c->ins[0], &v0) && v0 == 1) {
                    rebuf(c, c->ins[1]); chg++;
                } else if (is_cnst(M, c->ins[1], &v1) &&
                           (k = ispow2(v1)) >= 0) {
                    c->type = RT_SHL;
                    {
                        uint32_t drv = M->nets[c->ins[1]].driver;
                        if (drv > 0 && drv < M->n_cell)
                            M->cells[drv].param = (int64_t)k;
                    }
                    chg++;
                } else if (is_cnst(M, c->ins[0], &v0) &&
                           (k = ispow2(v0)) >= 0) {
                    uint32_t tmp = c->ins[0];
                    c->ins[0] = c->ins[1];
                    c->ins[1] = tmp;
                    c->type = RT_SHL;
                    {
                        uint32_t drv = M->nets[c->ins[1]].driver;
                        if (drv > 0 && drv < M->n_cell)
                            M->cells[drv].param = (int64_t)k;
                    }
                    chg++;
                } else if (is_cnst(M, c->ins[0], &v0) &&
                           is_cnst(M, c->ins[1], &v1)) {
                    fold(M, i, v0 * v1); chg++;
                }
            }
            break;

        case RT_MUX:
            if (c->n_in == 3) {
                if (is_cnst(M, c->ins[0], &v0)) {
                    rebuf(c, v0 ? c->ins[2] : c->ins[1]);
                    chg++;
                } else if (c->ins[1] == c->ins[2]) {
                    rebuf(c, c->ins[1]); chg++;
                } else if (is_cnst(M, c->ins[1], &v0) &&
                         is_cnst(M, c->ins[2], &v1) && v0 == v1) {
                    fold(M, i, v0); chg++;
                }
            }
            break;

        case RT_EQ:
            if (c->n_in == 2 &&
                is_cnst(M, c->ins[0], &v0) &&
                is_cnst(M, c->ins[1], &v1)) {
                fold(M, i, v0 == v1 ? 1 : 0); chg++;
            }
            break;

        case RT_NE:
            if (c->n_in == 2 &&
                is_cnst(M, c->ins[0], &v0) &&
                is_cnst(M, c->ins[1], &v1)) {
                fold(M, i, v0 != v1 ? 1 : 0); chg++;
            }
            break;

        case RT_LT:
            if (c->n_in == 2 &&
                is_cnst(M, c->ins[0], &v0) &&
                is_cnst(M, c->ins[1], &v1)) {
                fold(M, i, v0 < v1 ? 1 : 0); chg++;
            }
            break;

        case RT_MEMRD: case RT_MEMWR:
            break;

        default:
            break;
        }
    next:;
    }

    return chg;
}
/* op_pmatch moved to src/xform/tk_pmatch.c */
