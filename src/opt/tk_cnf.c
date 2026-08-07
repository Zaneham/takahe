/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_cnf.c -- turn a netlist into a question a SAT solver can answer
 *
 * Tseitin encoding. Every net becomes a variable, every cell becomes the
 * clauses that force its output to agree with its truth table. Ask for
 * an output to be true and the solver hands back the inputs that do it,
 * or proves no such inputs exist.
 *
 * Sequential designs get unrolled: k copies of the combinational logic,
 * with each copy's flop inputs wired to the previous copy's flop outputs.
 * Time becomes space, which is the oldest trick in the book and still
 * the best one available without a proper model checker.
 *
 * A cell with n inputs contributes 2^n clauses, one per row of its truth
 * table. Standard cells top out at five inputs, so that is 32 clauses at
 * worst and usually far fewer. Wasteful in theory, irrelevant in practice.
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Pool management ---- */

int
cn_init(cn_t *C, uint32_t max_cls, uint32_t max_lit)
{
    if (!C) return -1;
    memset(C, 0, sizeof(*C));
    C->lits = (int32_t *)malloc((size_t)max_lit * sizeof(int32_t));
    C->cls  = (uint32_t *)malloc(((size_t)max_cls + 1) * sizeof(uint32_t));
    if (!C->lits || !C->cls) { cn_free(C); return -1; }
    C->max_lit = max_lit;
    C->max_cls = max_cls;
    C->cls[0] = 0;
    return 0;
}

void
cn_free(cn_t *C)
{
    if (!C) return;
    free(C->lits); free(C->cls);
    memset(C, 0, sizeof(*C));
}

uint32_t
cn_var(cn_t *C)
{
    return ++C->n_var;
}

int
cn_add(cn_t *C, const int32_t *lits, uint32_t n)
{
    uint32_t i;

    if (!C || C->n_cls >= C->max_cls) return -1;
    if (C->n_lit + n > C->max_lit) return -1;

    for (i = 0; i < n; i++)
        C->lits[C->n_lit + i] = lits[i];
    C->n_lit += n;
    C->n_cls++;
    C->cls[C->n_cls] = C->n_lit;
    return 0;
}

int
cn_unit(cn_t *C, int32_t lit)
{
    return cn_add(C, &lit, 1);
}

/* ---- One cell, one truth table, 2^n clauses ----
 * For each row: if the inputs match that row, the output must take the
 * row's value. Written as a clause, "inputs match" gets negated, so the
 * row contributes (input mismatch) OR (output has the right value). */

int
cn_lut(cn_t *C, const cd_cell_t *t, uint8_t outsel,
       const uint32_t *ins, uint8_t n_in, uint32_t out)
{
    int32_t cl[RT_MAX_PIN + 1];
    uint16_t r;
    uint8_t j;

    if (!C || !t || out == 0) return -1;
    if (outsel >= t->n_out) return -1;
    if (n_in > RT_MAX_PIN) return -1;

    for (r = 0; r < t->n_row; r++) {
        const cd_row_t *row = &t->rows[r];
        uint8_t k = 0;
        int constant = 0;

        for (j = 0; j < n_in; j++) {
            if (ins[j] == 0) {
                /* Net with no variable: treat as a hard zero. If the row
                 * wants a one there the row can never fire. */
                if (row->ins[j]) { constant = 1; break; }
                continue;
            }
            /* Negate the "input equals row value" test */
            cl[k++] = row->ins[j] ? -(int32_t)ins[j] : (int32_t)ins[j];
        }
        if (constant) continue;

        cl[k++] = row->outs[outsel] ? (int32_t)out : -(int32_t)out;
        if (cn_add(C, cl, k) != 0) return -1;
    }
    return 0;
}

/* ---- DIMACS, for feeding an external solver or eyeballing ---- */

int
cn_dmcs(const cn_t *C, FILE *fp)
{
    uint32_t i, j;

    if (!C || !fp) return -1;
    fprintf(fp, "p cnf %u %u\n", C->n_var, C->n_cls);
    for (i = 0; i < C->n_cls; i++) {
        for (j = C->cls[i]; j < C->cls[i + 1]; j++)
            fprintf(fp, "%" PRId32 " ", C->lits[j]);
        fprintf(fp, "0\n");
    }
    return 0;
}

/* ---- Unroll ----
 * cur[] holds the variable for each net in the cycle being built. Flops
 * read cur[] from the previous cycle and drive the next, which is the
 * only place time enters the encoding. */

uint32_t
cn_unrl(cn_t *C, const rt_mod_t *M, const cd_lib_t *cd,
        uint32_t k, const uint32_t *inets, uint32_t n_in,
        uint32_t net, uint32_t *inv,
        const uint32_t *wnets, uint32_t n_w, uint32_t *wv)
{
    uint32_t *cur, *nxt, t, i, j;
    uint32_t res = 0;

    if (!C || !M || !cd || k == 0) return 0;

    cur = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    nxt = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    if (!cur || !nxt) { free(cur); free(nxt); return 0; }

    /* Reset state: a plain or resettable flop starts at zero, so it
     * gets no variable and folds away as a constant. A preset flop
     * starts at ONE, which needs a variable pinned true, because
     * "no variable" means zero everywhere else in this encoding. */
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type != RT_DFFS) continue;
        if (c->out == 0 || c->out >= M->n_net) continue;
        nxt[c->out] = cn_var(C);
        cn_unit(C, (int32_t)nxt[c->out]);
    }

    for (t = 0; t < k; t++) {
        /* Every net gets a new variable this cycle. Clearing only the
         * ports would leave the combinational nets holding last cycle's
         * variables, so no new logic would ever be encoded and the whole
         * unroll would collapse to one cycle wearing a disguise. */
        memset(cur, 0, (size_t)(M->n_net + 1) * sizeof(uint32_t));

        /* Fresh variables for the inputs we're solving for; every other
         * port stays at zero and folds away as a constant. */
        for (j = 0; j < n_in; j++) {
            if (inets[j] == 0 || inets[j] >= M->n_net) continue;
            cur[inets[j]] = cn_var(C);
            if (inv) inv[t * n_in + j] = cur[inets[j]];
        }

        /* Flop outputs for this cycle were computed last time round */
        for (i = 1; i < M->n_cell; i++) {
            const rt_cell_t *c = &M->cells[i];
            if (c->type != RT_DFF && c->type != RT_DFFR &&
                c->type != RT_DFFS) continue;
            if (c->out > 0 && c->out < M->n_net)
                cur[c->out] = nxt[c->out];
        }

        /* Combinational logic, in cell order. The reader emits cells in
         * netlist order, which is not topological, so sweep until the
         * variable assignment stops changing. */
        {
            uint32_t pass;
            KA_GUARD(g, 64);
            for (pass = 0; pass < 64 && g--; pass++) {
                int moved = 0;
                for (i = 1; i < M->n_cell; i++) {
                    const rt_cell_t *c = &M->cells[i];
                    if (c->type == RT_DFF || c->type == RT_DFFR ||
                        c->type == RT_DFFS) continue;
                    if (c->out == 0 || c->out >= M->n_net) continue;
                    if (cur[c->out] != 0) continue;
                    cur[c->out] = cn_var(C);
                    moved = 1;
                }
                if (!moved) break;
            }
        }

        /* Now every net has a variable, so emit the clauses */
        for (i = 1; i < M->n_cell; i++) {
            const rt_cell_t *c = &M->cells[i];
            uint32_t vin[RT_MAX_PIN];
            uint8_t pj;

            if (c->type == RT_DFF || c->type == RT_DFFR ||
                c->type == RT_DFFS) continue;
            if (c->type != RT_LUT) continue;
            if (c->out == 0 || c->out >= M->n_net) continue;
            if (c->cdix >= cd->n_cell) continue;

            for (pj = 0; pj < c->n_in; pj++)
                vin[pj] = (c->ins[pj] < M->n_net) ? cur[c->ins[pj]] : 0;
            if (cn_lut(C, &cd->cells[c->cdix], (uint8_t)c->param,
                       vin, c->n_in, cur[c->out]) != 0) {
                free(cur); free(nxt);
                return 0;
            }
        }

        /* Latch: next cycle's flop outputs are this cycle's D, unless
         * reset is held low, in which case they stay at zero. */
        memset(nxt, 0, (size_t)(M->n_net + 1) * sizeof(uint32_t));
        for (i = 1; i < M->n_cell; i++) {
            const rt_cell_t *c = &M->cells[i];
            if (c->type != RT_DFF && c->type != RT_DFFR &&
                c->type != RT_DFFS) continue;
            if (c->out == 0 || c->out >= M->n_net) continue;
            if (c->n_in > 0 && c->ins[0] < M->n_net)
                nxt[c->out] = cur[c->ins[0]];
        }

        /* Record the watched nets for this cycle, so the caller can
         * put constraints on what the design is doing over time and not
         * just on one signal at the end. */
        if (wv && wnets)
            for (j = 0; j < n_w; j++)
                wv[t * n_w + j] = (wnets[j] < M->n_net) ? cur[wnets[j]] : 0;

        if (t + 1 == k && net < M->n_net)
            res = cur[net];
    }

    free(cur); free(nxt);
    return res;
}

/* ---- Encoding a bit-level module ----
 * Rather than hand-write clauses per gate type and get one of them
 * subtly wrong, compute each gate's truth table as a bitmask and reuse
 * the same row-by-row clause generator the library cells go through.
 * One encoder, one place to be wrong. */

static int
cn_gmsk(rt_ctype_t t, uint8_t n_in, uint32_t *mask)
{
    uint32_t combo, m = 0;
    if (n_in == 0 || n_in > 5) return -1;

    for (combo = 0; combo < (1u << n_in); combo++) {
        uint32_t a = combo & 1u;
        uint32_t b = (combo >> 1) & 1u;
        uint32_t s = (combo >> 2) & 1u;
        uint32_t r;
        switch ((int)t) {
        case RT_AND:  r = a & b; break;
        case RT_OR:   r = a | b; break;
        case RT_XOR:  r = a ^ b; break;
        case RT_NAND: r = !(a & b); break;
        case RT_NOR:  r = !(a | b); break;
        case RT_XNOR: r = !(a ^ b); break;
        case RT_NOT:  r = !a; break;
        case RT_BUF:
        case RT_ASSIGN: r = a; break;
        case RT_MUX:  r = s ? b : a; break;
        default: return -1;
        }
        if (r) m |= 1u << combo;
    }
    *mask = m;
    return 0;
}

/* Emit clauses for an arbitrary truth-table mask over n_in inputs */
static int
cn_gate(cn_t *C, uint32_t mask, const uint32_t *ins, uint8_t n_in,
        uint32_t out)
{
    uint32_t combo;
    for (combo = 0; combo < (1u << n_in); combo++) {
        int32_t cl[RT_MAX_PIN + 1];
        uint8_t k = 0, j;
        int constant = 0;
        for (j = 0; j < n_in; j++) {
            uint32_t want = (combo >> j) & 1u;
            if (ins[j] == 0) { if (want) { constant = 1; break; } continue; }
            cl[k++] = want ? -(int32_t)ins[j] : (int32_t)ins[j];
        }
        if (constant) continue;
        cl[k++] = ((mask >> combo) & 1u) ? (int32_t)out : -(int32_t)out;
        if (cn_add(C, cl, k) != 0) return -1;
    }
    return 0;
}

int
cn_mod(cn_t *C, const rt_mod_t *M, const cd_lib_t *cd, uint32_t *vars)
{
    uint32_t i;

    if (!C || !M || !vars) return -1;
    memset(vars, 0, (size_t)(M->n_net + 1) * sizeof(uint32_t));

    /* Every net gets a variable. Constants get pinned below. */
    for (i = 1; i < M->n_net; i++) vars[i] = cn_var(C);

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        uint32_t vin[RT_MAX_PIN], mask;
        uint8_t j;

        if (c->type == RT_CELL_COUNT) continue;       /* dead */
        if (c->out == 0 || c->out >= M->n_net) continue;

        for (j = 0; j < c->n_in; j++)
            vin[j] = (c->ins[j] < M->n_net) ? vars[c->ins[j]] : 0;

        if (c->type == RT_CONST) {
            if (cn_unit(C, c->param ? (int32_t)vars[c->out]
                                    : -(int32_t)vars[c->out]) != 0)
                return -1;
            continue;
        }
        if (c->type == RT_LUT) {
            if (!cd || c->cdix >= cd->n_cell) return -1;
            if (cn_lut(C, &cd->cells[c->cdix], (uint8_t)c->param,
                       vin, c->n_in, vars[c->out]) != 0) return -1;
            continue;
        }
        /* Sequential and un-blasted wide operators are not
         * combinational, so say so rather than encode something that
         * happens to be satisfiable. */
        if (cn_gmsk(c->type, c->n_in, &mask) != 0) return -1;
        if (cn_gate(C, mask, vin, c->n_in, vars[c->out]) != 0) return -1;
    }
    return 0;
}

/* ---- Miter ----
 * Tie the two modules' inputs together by name, then assert that some
 * output pair differs. Unsatisfiable means no input tells them apart,
 * which is what equivalence means. */

static uint32_t
cn_fnet(const rt_mod_t *M, const char *nm, uint16_t nl)
{
    uint32_t i;
    for (i = 1; i < M->n_net; i++)
        if (M->nets[i].name_len == nl &&
            memcmp(M->strs + M->nets[i].name_off, nm, nl) == 0)
            return i;
    return 0;
}

uint32_t
cn_mitr(cn_t *C, const rt_mod_t *A, const rt_mod_t *B, const cd_lib_t *cd)
{
    uint32_t *va, *vb, i, diff = 0, n_diff = 0;
    int32_t *any;

    if (!C || !A || !B) return 0;
    va = (uint32_t *)calloc(A->n_net + 1, sizeof(uint32_t));
    vb = (uint32_t *)calloc(B->n_net + 1, sizeof(uint32_t));
    any = (int32_t *)calloc(A->n_net + 1, sizeof(int32_t));
    if (!va || !vb || !any) { free(va); free(vb); free(any); return 0; }

    if (cn_mod(C, A, cd, va) != 0 || cn_mod(C, B, cd, vb) != 0) {
        free(va); free(vb); free(any);
        return 0;
    }

    /* Inputs shared: force the two copies to agree */
    for (i = 1; i < A->n_net; i++) {
        uint32_t bi;
        if (A->nets[i].is_port != 1) continue;
        bi = cn_fnet(B, A->strs + A->nets[i].name_off,
                     A->nets[i].name_len);
        if (!bi) continue;
        {
            int32_t c1[2], c2[2];
            c1[0] = -(int32_t)va[i]; c1[1] = (int32_t)vb[bi];
            c2[0] = (int32_t)va[i];  c2[1] = -(int32_t)vb[bi];
            if (cn_add(C, c1, 2) != 0 || cn_add(C, c2, 2) != 0) goto fail;
        }
    }

    /* Outputs: one XOR per pair, then assert at least one is true */
    for (i = 1; i < A->n_net; i++) {
        uint32_t bi, x;
        uint32_t ins2[2];
        if (A->nets[i].is_port != 2) continue;
        bi = cn_fnet(B, A->strs + A->nets[i].name_off,
                     A->nets[i].name_len);
        if (!bi) continue;
        x = cn_var(C);
        ins2[0] = va[i]; ins2[1] = vb[bi];
        if (cn_gate(C, 0x6u, ins2, 2, x) != 0) goto fail;   /* XOR */
        any[n_diff++] = (int32_t)x;
    }

    if (n_diff == 0) goto fail;   /* nothing comparable, say nothing */
    diff = cn_var(C);
    {
        int32_t *cl = (int32_t *)malloc((size_t)(n_diff + 1) *
                                        sizeof(int32_t));
        uint32_t k;
        if (!cl) goto fail;
        for (k = 0; k < n_diff; k++) cl[k] = any[k];
        cl[n_diff] = -(int32_t)diff;
        if (cn_add(C, cl, n_diff + 1) != 0) { free(cl); goto fail; }
        for (k = 0; k < n_diff; k++) {
            int32_t two[2];
            two[0] = -any[k]; two[1] = (int32_t)diff;
            if (cn_add(C, two, 2) != 0) { free(cl); goto fail; }
        }
        free(cl);
    }
    if (cn_unit(C, (int32_t)diff) != 0) goto fail;

    free(va); free(vb); free(any);
    return diff;
fail:
    free(va); free(vb); free(any);
    return 0;
}
