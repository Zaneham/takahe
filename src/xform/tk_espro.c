/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_espro.c -- Espresso integration for Takahe RTL
 *
 * Walks output nets, extracts small logic cones (≤ ES_MAXIN
 * inputs), minimises them with Espresso, and replaces the
 * original gate cloud with a clean two-level AND-OR network.
 *
 * The original cone might be 30 gates of tangled MUXes and
 * XORs from bit-blast. After Espresso it's 4 product terms
 * feeding one OR gate. Same function, fewer transistors,
 * lower area, faster timing, and the layout engineer buys
 * you a coffee.
 *
 * "Now he's thinking bout me every night, oh,
 *  is it that sweet? I guess so."
 *   — The circuit, after minimisation.
 */

#include "takahe.h"

/* ---- Espresso cover (must match tk_espresso.c internal) ---- */

typedef struct {
    uint32_t pos;
    uint32_t neg;
} eo_cube_t;

typedef struct {
    eo_cube_t cubes[ES_MAXCUB];
    int       n;
    int       nin;
} eo_cover_t;

/* ---- Mark all cells in a cone as dead ---- */

static void
eo_kill(rt_mod_t *M, uint32_t out_net,
        const uint32_t *inputs, int nin)
{
    /* BFS backward from out_net, kill all cells in the cone.
     * Stop at input nets (don't kill their drivers). */
    uint32_t stk[256];
    uint8_t  *vis;
    int sp = 0, i;

    vis = (uint8_t *)calloc(M->n_net, 1);
    if (!vis) return;

    stk[sp++] = out_net;
    vis[out_net] = 1;

    KA_GUARD(gk, 1000);
    while (sp > 0 && gk--) {
        uint32_t ni = stk[--sp];
        uint32_t drv = M->nets[ni].driver;
        uint8_t j;

        /* Don't kill past input boundaries */
        for (i = 0; i < nin; i++) {
            if (inputs[i] == ni) goto skip_net;
        }

        if (drv == 0 || drv >= M->n_cell) continue;
        {
            rt_cell_t *c = &M->cells[drv];
            if (c->type == RT_CELL_COUNT) continue;
            if (c->type == RT_DFF || c->type == RT_DFFR) continue;

            /* Kill this cell */
            c->type = RT_CELL_COUNT;

            /* Push its inputs */
            for (j = 0; j < c->n_in && j < RT_MAX_PIN; j++) {
                uint32_t inp = c->ins[j];
                if (inp > 0 && inp < M->n_net &&
                    !vis[inp]) {
                    vis[inp] = 1;
                    if (sp < 256) stk[sp++] = inp;
                }
            }
        }
    skip_net:;
    }
    free(vis);
}

/* ---- Build AND-OR network from Espresso cover ---- */

static int
eo_build(rt_mod_t *M, uint32_t out_net,
         const uint32_t *inputs, int nin,
         const eo_cover_t *cover)
{
    uint32_t terms[ES_MAXCUB]; /* AND gate outputs */
    int ci, created = 0;

    if (cover->n == 0) return 0;

    /* Single product term → direct AND/ASSIGN to output */
    /* Multiple terms → AND gates + OR tree */

    for (ci = 0; ci < cover->n && ci < ES_MAXCUB; ci++) {
        const eo_cube_t *cube = &cover->cubes[ci];
        uint32_t lits[ES_MAXIN];
        int nlits = 0;
        int i;

        /* Collect literals for this product term */
        for (i = 0; i < nin; i++) {
            if ((cube->pos >> i) & 1) {
                /* Positive literal: input i directly */
                lits[nlits++] = inputs[i];
            } else if ((cube->neg >> i) & 1) {
                /* Negative literal: NOT(input i) */
                uint32_t inv = rt_anet(M, "ei", 2, 1, 0,
                                       TK_RADIX_BIN);
                uint32_t ins[1] = { inputs[i] };
                rt_acell(M, RT_NOT, inv, ins, 1, 1);
                lits[nlits++] = inv;
                created++;
            }
            /* else: don't-care, not in this term */
        }

        if (nlits == 0) {
            /* All don't-care = constant 1 */
            terms[ci] = rt_anet(M, "e1", 2, 1, 0, TK_RADIX_BIN);
            {
                uint32_t cc = rt_acell(M, RT_CONST, terms[ci],
                                       NULL, 0, 1);
                if (cc > 0 && cc < M->n_cell)
                    M->cells[cc].param = 1;
            }
            created++;
        } else if (nlits == 1) {
            /* Single literal = just a wire */
            terms[ci] = lits[0];
        } else {
            /* Multi-literal AND: chain of 2-input ANDs */
            uint32_t acc = lits[0];
            int j;
            for (j = 1; j < nlits; j++) {
                uint32_t an = rt_anet(M, "ea", 2, 1, 0,
                                      TK_RADIX_BIN);
                uint32_t ins[2] = { acc, lits[j] };
                rt_acell(M, RT_AND, an, ins, 2, 1);
                acc = an;
                created++;
            }
            terms[ci] = acc;
        }
    }

    /* OR tree: combine product terms */
    if (cover->n == 1) {
        /* Single term → assign to output */
        uint32_t ins[1] = { terms[0] };
        rt_acell(M, RT_ASSIGN, out_net, ins, 1, 1);
        created++;
    } else {
        /* OR chain */
        uint32_t acc = terms[0];
        int j;
        for (j = 1; j < cover->n; j++) {
            if (j == cover->n - 1) {
                /* Last OR drives output directly */
                uint32_t ins[2] = { acc, terms[j] };
                rt_acell(M, RT_OR, out_net, ins, 2, 1);
            } else {
                uint32_t orn = rt_anet(M, "eo", 2, 1, 0,
                                       TK_RADIX_BIN);
                uint32_t ins[2] = { acc, terms[j] };
                rt_acell(M, RT_OR, orn, ins, 2, 1);
                acc = orn;
            }
            created++;
        }
    }

    return created;
}

/* ---- Public: run Espresso on all small output cones ---- */

int
op_espro(rt_mod_t *M)
{
    uint32_t i;
    int total = 0;

    if (!M) return 0;

    KA_GUARD(gnet, 262144);
    for (i = 1; i < M->n_net && gnet--; i++) {
        const rt_net_t *n = &M->nets[i];
        uint32_t onm[ES_MAXCUB], offm[ES_MAXCUB];
        uint32_t inputs[ES_MAXIN];
        int non = 0, noff = 0, nin = 0;
        eo_cover_t cover;
        int nterms, rc;

        /* Minimise any 1-bit net with a driver.
         * Skip inputs (no driver) and undriven nets. */
        if (n->width != 1) continue;
        if (n->is_port == 1) continue; /* input port — no cone */
        if (n->driver == 0) continue;  /* undriven */

        /* Quick check: is this net driven by a complex cone?
         * Skip if driver is ASSIGN/CONST/BUF (trivial). */
        {
            uint32_t drv = n->driver;
            if (drv > 0 && drv < M->n_cell) {
                rt_ctype_t dt = M->cells[drv].type;
                if (dt == RT_ASSIGN || dt == RT_BUF ||
                    dt == RT_CONST || dt == RT_CELL_COUNT)
                    continue;
            }
        }

        /* Extract cone */
        rc = es_cone(M, i, onm, &non, offm, &noff,
                     inputs, &nin);
        if (rc < 0 || nin < 2 || nin > 8) continue;
        /* Skip tiny cones (not worth it) and large ones
         * (>8 inputs = 256 minterms, keeps it fast) */

        if (non == 0) continue; /* constant 0 — cprop handles it */

        /* Minimise */
        nterms = es_mini(onm, non, offm, noff, nin, &cover);
        if (nterms <= 0) continue;

        /* Is the minimised version actually smaller?
         * Count gates in original cone vs product terms + OR */
        {
            int orig_gates = non; /* rough: one gate per minterm */
            int new_gates = 0;
            int ci2;
            for (ci2 = 0; ci2 < nterms; ci2++) {
                int lc = 0, li;
                for (li = 0; li < nin; li++) {
                    if ((cover.cubes[ci2].pos >> li) & 1) lc++;
                    if ((cover.cubes[ci2].neg >> li) & 1) lc++;
                }
                new_gates += lc; /* AND chain length */
            }
            new_gates += nterms - 1; /* OR tree */

            if (new_gates >= orig_gates) continue; /* no improvement */
        }

        /* Kill original cone and build minimised version.
         * Journaled: if the build fails, roll back the kill.
         * CICS pattern: either the whole transaction completes,
         * or none of it does. No half-dead cones. */
        jr_begin(M);
        eo_kill(M, i, inputs, nin);
        {
            int built = eo_build(M, i, inputs, nin, &cover);
            if (built > 0) {
                jr_commit();
                total++;
            } else {
                jr_rback(M, NULL);
            }
        }
    }

    if (total > 0)
        printf("takahe: espresso: %d cones minimised\n", total);
    return total;
}
