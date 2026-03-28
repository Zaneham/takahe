/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_espresso.c -- Two-level logic minimisation for Takahe
 *
 * "That's that me espresso."
 *   — Carpenter, S. (2024). Espresso [Song]. On Short n' Sweet.
 *     Island Records.
 *
 * "Is it that sweet? I guess so."
 *   — Also Sabrina Carpenter, but she could have been talking
 *     about a minimal sum-of-products cover, because it IS
 *     that sweet when you eliminate 40% of your product terms
 *     and the circuit area drops like a pop single climbing
 *     the charts in reverse.
 *
 * The algorithm is Espresso-II from Berkeley:
 *   EXPAND  — make each cube as large as possible
 *   IRREDUNDANT — remove cubes the cover doesn't need
 *   REDUCE  — shrink cubes to create new expansion opportunities
 *   Repeat until the cover stops improving, or until you've
 *   been working late, cause you're a minimiser.
 *
 * References (APA 7th):
 *
 * Brayton, R. K., Hachtel, G. D., McMullen, C. T., &
 *   Sangiovanni-Vincentelli, A. L. (1984). Logic minimization
 *   algorithms for VLSI synthesis. Kluwer Academic Publishers.
 *   https://doi.org/10.1007/978-1-4613-2821-6
 *
 * Rudell, R. L., & Sangiovanni-Vincentelli, A. L. (1987).
 *   Multiple-valued minimization for PLA optimization. IEEE
 *   Transactions on Computer-Aided Design of Integrated
 *   Circuits and Systems, 6(5), 727–750.
 *   https://doi.org/10.1109/TCAD.1987.1270318
 *
 * McGeer, P. C., Sanghavi, J. V., Brayton, R. K., &
 *   Sangiovanni-Vincentelli, A. L. (1993). ESPRESSO-SIGNATURE:
 *   A new exact minimizer for logic functions. IEEE Transactions
 *   on VLSI Systems, 1(4), 432–440.
 *
 * Implementation note: this is a simplified Espresso for
 * functions up to ES_MAXIN inputs (16). Larger cones are
 * left unminimised — the gate-level pattern matcher handles
 * those. Sixteen inputs covers the vast majority of practical
 * logic cones in synthesised designs.
 */

#include "takahe.h"

/* ---- Cube representation ----
 * Each input is 2 bits: 00=don't-care, 01=positive, 10=negative.
 * A cube is a product term. A cover is a set of cubes.
 * Stored as uint32_t bitmasks for up to 16 inputs.
 * pos_mask bit i = 1 means input i must be 1.
 * neg_mask bit i = 1 means input i must be 0.
 * Both 0 = don't-care. Both 1 = impossible (empty). */

#define ES_MAXIN  16
#define ES_MAXCUB 256

typedef struct {
    uint32_t pos;   /* positive literals */
    uint32_t neg;   /* negative literals */
} es_cube_t;

typedef struct {
    es_cube_t cubes[ES_MAXCUB];
    int       n;
    int       nin;  /* number of inputs */
} es_cover_t;

/* ---- Does cube c cover minterm m? ---- */

static int
es_covers(const es_cube_t *c, uint32_t m, int nin)
{
    int i;
    for (i = 0; i < nin; i++) {
        uint32_t b = (m >> i) & 1;
        if ((c->pos >> i) & 1) { if (!b) return 0; }
        if ((c->neg >> i) & 1) { if (b) return 0; }
    }
    return 1;
}

/* ---- EXPAND: make each cube as large as possible ----
 * For each literal in each cube, try removing it.
 * If the expanded cube doesn't cover any OFF-set minterms,
 * keep the expansion. Greedy, not optimal, but fast.
 * "Now he's thinking bout me every night" — the cube
 * keeps expanding until it can't anymore. */

static int
es_expand(es_cover_t *F, const uint32_t *offm, int noff)
{
    int ci, i, changed = 0;

    for (ci = 0; ci < F->n; ci++) {
        es_cube_t *c = &F->cubes[ci];
        for (i = 0; i < F->nin; i++) {
            es_cube_t trial = *c;
            int covers_off = 0;
            int j;

            /* Try removing positive literal i */
            if ((c->pos >> i) & 1) {
                trial.pos &= ~(1u << i);
                for (j = 0; j < noff; j++) {
                    if (es_covers(&trial, offm[j], F->nin)) {
                        covers_off = 1; break;
                    }
                }
                if (!covers_off) { *c = trial; changed++; }
            }

            /* Try removing negative literal i */
            covers_off = 0;
            trial = *c;
            if ((c->neg >> i) & 1) {
                trial.neg &= ~(1u << i);
                for (j = 0; j < noff; j++) {
                    if (es_covers(&trial, offm[j], F->nin)) {
                        covers_off = 1; break;
                    }
                }
                if (!covers_off) { *c = trial; changed++; }
            }
        }
    }
    return changed;
}

/* ---- IRREDUNDANT: remove redundant cubes ----
 * A cube is redundant if every minterm it covers is also
 * covered by other cubes in the cover. Remove it.
 * "Say you can't sleep, baby, I know" — the redundant
 * cube wasn't doing anything anyway. */

static int
es_irred(es_cover_t *F, const uint32_t *onm, int non)
{
    int ci, removed = 0;

    for (ci = F->n - 1; ci >= 0; ci--) {
        /* Check: are all minterms covered by this cube
         * also covered by OTHER cubes? */
        int essential = 0;
        int j;

        for (j = 0; j < non; j++) {
            if (es_covers(&F->cubes[ci], onm[j], F->nin)) {
                /* Is this minterm covered by any other cube? */
                int other = 0;
                int k;
                for (k = 0; k < F->n; k++) {
                    if (k == ci) continue;
                    if (es_covers(&F->cubes[k], onm[j], F->nin)) {
                        other = 1; break;
                    }
                }
                if (!other) { essential = 1; break; }
            }
        }

        if (!essential && F->n > 1) {
            /* Remove cube by swapping with last */
            F->cubes[ci] = F->cubes[F->n - 1];
            F->n--;
            removed++;
        }
    }
    return removed;
}

/* ---- REDUCE: shrink cubes to create expansion opportunities ----
 * For each cube, try adding back literals. If the cover
 * still covers the ON-set, keep the smaller cube.
 * This creates opportunities for EXPAND to find better
 * expansions in the next iteration.
 * "I can see you starin', honey" — the reduced cube
 * is smaller but it's looking at new opportunities. */

static int
es_reduce(es_cover_t *F, const uint32_t *onm, int non)
{
    int ci, i, changed = 0;

    for (ci = 0; ci < F->n; ci++) {
        es_cube_t *c = &F->cubes[ci];
        for (i = 0; i < F->nin; i++) {
            /* Skip if already has this literal */
            if (((c->pos >> i) & 1) || ((c->neg >> i) & 1))
                continue;

            /* Try adding positive literal */
            {
                es_cube_t trial = *c;
                int all_covered = 1;
                int j;
                trial.pos |= (1u << i);

                /* Check: does the cover still cover all ON-set? */
                for (j = 0; j < non; j++) {
                    if (es_covers(c, onm[j], F->nin) &&
                        !es_covers(&trial, onm[j], F->nin)) {
                        /* This minterm was covered by c but not by trial.
                         * Is it covered by another cube? */
                        int k, other = 0;
                        for (k = 0; k < F->n; k++) {
                            if (k == ci) continue;
                            if (es_covers(&F->cubes[k], onm[j], F->nin)) {
                                other = 1; break;
                            }
                        }
                        if (!other) { all_covered = 0; break; }
                    }
                }
                if (all_covered) { *c = trial; changed++; }
            }
        }
    }
    return changed;
}

/* ---- Public: minimise a truth table ----
 * Takes an ON-set (minterms where output = 1) and an OFF-set
 * (minterms where output = 0). DC-set is implicit (everything
 * not in ON or OFF).
 *
 * Returns minimised cover in F. Returns number of product terms.
 *
 * "Boy, I'm working late, cause I'm a minimiser." */

int
es_mini(const uint32_t *onm, int non, const uint32_t *offm,
        int noff, int nin, void *cover)
{
    es_cover_t *F = (es_cover_t *)cover;
    int i, iter;

    if (!F || nin > ES_MAXIN || non > ES_MAXCUB) return -1;

    memset(F, 0, sizeof(*F));
    F->nin = nin;

    /* Initialise: one cube per ON-set minterm (trivial cover) */
    for (i = 0; i < non && i < ES_MAXCUB; i++) {
        int j;
        F->cubes[i].pos = 0;
        F->cubes[i].neg = 0;
        for (j = 0; j < nin; j++) {
            if ((onm[i] >> j) & 1)
                F->cubes[i].pos |= (1u << j);
            else
                F->cubes[i].neg |= (1u << j);
        }
    }
    F->n = non < ES_MAXCUB ? non : ES_MAXCUB;

    /* Espresso main loop: expand → irredundant → reduce.
     * Iterate until no improvement. Like a pop chorus:
     * repeat until it's stuck in your head. */
    KA_GUARD(giter, 20);
    for (iter = 0; giter--; iter++) {
        (void)iter;
        int e = es_expand(F, offm, noff);
        int r2 = es_irred(F, onm, non);
        int d = es_reduce(F, onm, non);
        if (e == 0 && r2 == 0 && d == 0) break;
    }

    /* Final irredundant pass */
    es_irred(F, onm, non);

    return F->n;
}

/* ---- Extract truth table from a logic cone ----
 * Given an output net, trace back through gates to find
 * all primary inputs (≤ ES_MAXIN), compute the truth table
 * by evaluating each minterm, and return ON/OFF sets.
 *
 * Returns number of inputs, or -1 if cone is too large. */

int
es_cone(const rt_mod_t *M, uint32_t out_net,
        uint32_t *onm, int *non,
        uint32_t *offm, int *noff,
        uint32_t *inputs, int *nin)
{
    /* Trace the fanin cone of out_net */
    uint32_t stk[256];     /* BFS stack */
    uint8_t  *visited;     /* net visited flags */
    int sp = 0, ni = 0;
    uint32_t i;

    if (!M || out_net == 0 || out_net >= M->n_net) return -1;

    visited = (uint8_t *)calloc(M->n_net, 1);
    if (!visited) return -1;
    *non = 0; *noff = 0; *nin = 0;

    /* BFS from output net back through drivers */
    stk[sp++] = out_net;
    visited[out_net] = 1;

    KA_GUARD(gbfs, 1000);
    while (sp > 0 && gbfs--) {
        uint32_t ni2 = stk[--sp];
        uint32_t drv = M->nets[ni2].driver;
        uint8_t j;

        if (drv == 0 || drv >= M->n_cell) {
            /* No driver = primary input */
            if (ni < ES_MAXIN)
                inputs[ni++] = ni2;
            else
                { free(visited); return -1; }
            continue;
        }

        {
            const rt_cell_t *c = &M->cells[drv];
            if (c->type == RT_CELL_COUNT) continue;

            /* DFFs are timing boundaries — treat as primary inputs */
            if (c->type == RT_DFF || c->type == RT_DFFR) {
                if (ni < ES_MAXIN)
                    inputs[ni++] = ni2;
                else
                    { free(visited); return -1; }
                continue;
            }

            /* Add all inputs of this cell to the BFS */
            for (j = 0; j < c->n_in && j < RT_MAX_PIN; j++) {
                uint32_t inp = c->ins[j];
                if (inp > 0 && inp < M->n_net &&
                    !visited[inp]) {
                    visited[inp] = 1;
                    if (sp < 256) stk[sp++] = inp;
                }
            }
        }
    }

    *nin = ni;
    if (ni == 0 || ni > ES_MAXIN) { free(visited); return -1; }

    /* Collect cone cells during BFS (they're the visited ones
     * that have drivers). Only simulate these, not the whole
     * netlist. The difference: 50 cells vs 40,000. */
    {
        uint32_t cone_cells[256];
        int ncone = 0;
        KA_GUARD(gcc2, 262144);
        for (i = 1; i < M->n_cell && ncone < 256 && gcc2--; i++) {
            const rt_cell_t *cc = &M->cells[i];
            if (cc->type == RT_CELL_COUNT) continue;
            if (cc->type == RT_DFF || cc->type == RT_DFFR) continue;
            if (cc->out > 0 && cc->out < M->n_net && visited[cc->out])
                cone_cells[ncone++] = i;
        }

    /* Evaluate truth table by simulating cone cells only. */
    {
        uint32_t nmint = 1u << ni;
        uint32_t m;
        uint8_t *netval;

        if (nmint > 65536) { free(visited); return -1; }

        netval = (uint8_t *)calloc(M->n_net, 1);
        if (!netval) { free(visited); return -1; }

        KA_GUARD(gm, 65536);
        for (m = 0; m < nmint && gm--; m++) {
            int j2;

            /* Set input values */
            memset(netval, 0, M->n_net);
            for (j2 = 0; j2 < ni; j2++) {
                netval[inputs[j2]] = (uint8_t)((m >> j2) & 1);
            }

            /* Propagate: evaluate only cells in the cone. */
            {
                int pass;
                KA_GUARD(gp, 50);
                for (pass = 0; gp--; pass++) {
                    (void)pass;
                    int chg = 0;
                    int ci2;
                    for (ci2 = 0; ci2 < ncone; ci2++) {
                        const rt_cell_t *c2 = &M->cells[cone_cells[ci2]];
                        uint8_t a, b, r;
                        if (c2->type == RT_CELL_COUNT) continue;
                        if (c2->out == 0 || c2->out >= M->n_net) continue;
                        if (c2->type == RT_DFF || c2->type == RT_DFFR) continue;

                        a = c2->n_in >= 1 && c2->ins[0] < M->n_net ?
                            netval[c2->ins[0]] : 0;
                        b = c2->n_in >= 2 && c2->ins[1] < M->n_net ?
                            netval[c2->ins[1]] : 0;
                        r = 0;

                        switch ((int)c2->type) {
                        case RT_AND: case RT_NAND:
                            r = a & b;
                            if (c2->type == RT_NAND) r = !r;
                            break;
                        case RT_OR: case RT_NOR:
                            r = a | b;
                            if (c2->type == RT_NOR) r = !r;
                            break;
                        case RT_XOR: case RT_XNOR:
                            r = a ^ b;
                            if (c2->type == RT_XNOR) r = !r;
                            break;
                        case RT_NOT:
                            r = !a; break;
                        case RT_BUF: case RT_ASSIGN:
                            r = a; break;
                        case RT_MUX:
                        {
                            uint8_t s = a;
                            uint8_t d0 = b;
                            uint8_t d1 = c2->n_in >= 3 &&
                                c2->ins[2] < M->n_net ?
                                netval[c2->ins[2]] : 0;
                            r = s ? d1 : d0;
                            break;
                        }
                        case RT_CONST:
                            r = c2->param ? 1 : 0; break;
                        default:
                            r = 0; break;
                        }

                        if (netval[c2->out] != r) {
                            netval[c2->out] = r;
                            chg = 1;
                        }
                    }
                    if (!chg) break;
                }
            }

            /* Read output value */
            if (netval[out_net]) {
                if (*non < ES_MAXCUB) onm[(*non)++] = m;
            } else {
                if (*noff < ES_MAXCUB) offm[(*noff)++] = m;
            }
        }

        free(netval);
    }
    } /* close cone_cells scope */

    free(visited);
    return ni;
}
