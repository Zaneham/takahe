/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_dce.c -- Dead cell elimination for Takahe RTL
 *
 * If nobody reads your output and you're not a port,
 * you're dead weight. This pass finds cells whose output
 * nets have zero fanout and sends them to the knacker's
 * yard. Iterates to fixpoint because killing one cell
 * may orphan another's inputs.
 *
 * Like a municipal council: if nobody uses the road,
 * eventually someone notices and stops maintaining it.
 */

#include "takahe.h"

/* ---- Public: compute fanout array ---- */

uint32_t *
rt_fan(rt_mod_t *M)
{
    uint32_t *fan;
    uint32_t i;
    uint8_t j;

    if (!M) return NULL;
    fan = (uint32_t *)calloc(M->n_net, sizeof(uint32_t));
    if (!fan) return NULL;

    /* Mark port nets as having external fanout */
    for (i = 1; i < M->n_net; i++) {
        if (M->nets[i].is_port == 2 || M->nets[i].is_port == 3)
            fan[i] = 1;  /* output/inout: external consumer */
    }

    /* Count internal fanout from cell inputs */
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type == RT_CELL_COUNT) continue;
        for (j = 0; j < c->n_in; j++) {
            uint32_t ni = c->ins[j];
            if (ni > 0 && ni < M->n_net)
                fan[ni]++;
        }
    }

    return fan;
}

/* ---- Public: dead cell elimination ---- */

int
op_dce(rt_mod_t *M)
{
    int chg = 0;
    uint32_t *fan;
    uint32_t i;
    uint8_t j;
    KA_GUARD(iter, 100);

    if (!M) return 0;

    while (iter--) {
        int rnd = 0;

        fan = rt_fan(M);
        if (!fan) break;

        for (i = 1; i < M->n_cell; i++) {
            rt_cell_t *c = &M->cells[i];
            uint32_t o;

            if (c->type == RT_CELL_COUNT) continue;
            o = c->out;

            /* Skip if output is a port (external) */
            if (o > 0 && o < M->n_net &&
                (M->nets[o].is_port == 2 || M->nets[o].is_port == 3))
                continue;

            /* MEMWR is side-effecting — never kill it */
            if (c->type == RT_MEMWR) continue;

            /* Dead if output has zero fanout */
            if (o > 0 && o < M->n_net && fan[o] == 0) {
                /* Remove cell's contribution to input fanout
                 * (for the next iteration's count) */
                for (j = 0; j < c->n_in; j++) {
                    uint32_t ni = c->ins[j];
                    if (ni > 0 && ni < M->n_net && fan[ni] > 0)
                        fan[ni]--;
                }
                c->type = RT_CELL_COUNT;  /* mark dead */
                rnd++;
            }
        }

        free(fan);
        chg += rnd;
        if (rnd == 0) break;
    }

    return chg;
}
