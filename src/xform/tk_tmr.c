/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_tmr.c -- Triple Modular Redundancy for Takahe
 *
 * Triplicates sequential elements and inserts majority voters.
 * If radiation flips one flip-flop, the other two outvote it.
 * The same trick Voyager uses, except now your synthesiser
 * does it for you instead of making you do it by hand.
 *
 * The voter is just (a & b) | (b & c) | (a & c). Three ANDs,
 * two ORs. No new cell types, no special handling anywhere
 * in the pipeline. The optimiser, mapper, FPGA backend, and
 * emitters all work without changes because a voter is just
 * combinational logic wearing a fancy hat.
 */

#include "takahe.h"

/* ---- Create a net with a suffixed name ---- */

static uint32_t
tm_net(rt_mod_t *M, uint32_t orig, const char *sfx)
{
    char name[128];
    int nlen;

    if (orig >= M->n_net) return 0;

    nlen = snprintf(name, sizeof(name), "%.*s%s",
                    (int)M->nets[orig].name_len,
                    M->strs + M->nets[orig].name_off,
                    sfx);

    return rt_anet(M, name, (uint16_t)nlen,
                   M->nets[orig].width, 0, 2);
}

/* ---- Build a majority voter: (a&b)|(b&c)|(a&c) ----
 * Returns the output net index. */

static uint32_t
tm_vote(rt_mod_t *M, uint32_t na, uint32_t nb, uint32_t nc,
        uint32_t width)
{
    uint32_t nt0, nt1, nt2, nt3, out;
    uint32_t ins2[2];

    nt0 = rt_anet(M, "", 0, width, 0, 2);
    nt1 = rt_anet(M, "", 0, width, 0, 2);
    nt2 = rt_anet(M, "", 0, width, 0, 2);
    nt3 = rt_anet(M, "", 0, width, 0, 2);
    out = rt_anet(M, "", 0, width, 0, 2);
    if (!nt0 || !nt1 || !nt2 || !nt3 || !out) return 0;

    /* t0 = a & b */
    ins2[0] = na; ins2[1] = nb;
    if (!rt_acell(M, RT_AND, nt0, ins2, 2, width)) return 0;

    /* t1 = b & c */
    ins2[0] = nb; ins2[1] = nc;
    if (!rt_acell(M, RT_AND, nt1, ins2, 2, width)) return 0;

    /* t2 = a & c */
    ins2[0] = na; ins2[1] = nc;
    if (!rt_acell(M, RT_AND, nt2, ins2, 2, width)) return 0;

    /* t3 = t0 | t1 */
    ins2[0] = nt0; ins2[1] = nt1;
    if (!rt_acell(M, RT_OR, nt3, ins2, 2, width)) return 0;

    /* out = t3 | t2 */
    ins2[0] = nt3; ins2[1] = nt2;
    if (!rt_acell(M, RT_OR, out, ins2, 2, width)) return 0;

    return out;
}

/* ---- Triplicate one DFF and insert voter ---- */

static int
tm_dff(rt_mod_t *M, uint32_t ci)
{
    rt_cell_t *orig;
    uint32_t orig_out;
    uint32_t net_a, net_b, net_c;
    uint32_t voter_out;
    uint32_t width;

    if (ci >= M->n_cell) return -1;
    orig = &M->cells[ci];
    orig_out = orig->out;

    if (orig_out == 0 || orig_out >= M->n_net) return -1;
    width = M->nets[orig_out].width;
    if (width == 0) width = 1;

    /* Create three replica output nets */
    net_a = tm_net(M, orig_out, "_A");
    net_b = tm_net(M, orig_out, "_B");
    net_c = tm_net(M, orig_out, "_C");
    if (!net_a || !net_b || !net_c) return -1;

    /* Create DFF_B and DFF_C replicas (same type, inputs, width) */
    if (!rt_acell(M, orig->type, net_b,
                  orig->ins, orig->n_in, width)) return -1;
    if (!rt_acell(M, orig->type, net_c,
                  orig->ins, orig->n_in, width)) return -1;

    /* Rewire original DFF to output net_a */
    orig->out = net_a;

    /* Build voter, outputs to a new net */
    voter_out = tm_vote(M, net_a, net_b, net_c, width);
    if (!voter_out) return -1;

    /* Rewire: voter output drives the original net.
     * Find the voter's last OR cell and point it at orig_out.
     * All downstream consumers still read orig_out. */
    M->cells[M->n_cell - 1].out = orig_out;

    return 0;
}

/* ---- Public: run TMR pass ---- */

int
tm_tmr(rt_mod_t *M, int full)
{
    uint32_t i, snap;
    int count = 0;

    if (!M) return -1;

    snap = M->n_cell;

    /* Count targets and check headroom */
    {
        uint32_t ndff = 0;
        for (i = 1; i < snap; i++) {
            rt_ctype_t t = M->cells[i].type;
            if (full) {
                if (t != RT_CELL_COUNT && M->cells[i].out > 0)
                    ndff++;
            } else {
                if (t == RT_DFF || t == RT_DFFR)
                    ndff++;
            }
        }

        if (M->n_cell + ndff * 9 >= M->max_cell ||
            M->n_net + ndff * 8 >= M->max_net) {
            fprintf(stderr,
                "takahe: TMR: pool overflow (%u cells, "
                "need ~%u more)\n",
                M->n_cell, ndff * 9);
            return -1;
        }

        printf("takahe: TMR: %u %s to triplicate\n",
               ndff, full ? "cells" : "DFFs");
    }

    for (i = 1; i < snap; i++) {
        rt_ctype_t t = M->cells[i].type;
        int target = 0;

        if (full)
            target = (t != RT_CELL_COUNT && M->cells[i].out > 0);
        else
            target = (t == RT_DFF || t == RT_DFFR);

        if (target) {
            if (tm_dff(M, i) != 0) {
                fprintf(stderr,
                    "takahe: TMR: failed at cell %u\n", i);
                return -1;
            }
            count++;
        }
    }

    printf("takahe: TMR: %d %s triplicated, %d voters\n",
           count, full ? "cells" : "DFFs", count);
    printf("takahe: TMR: %u total cells (was %u)\n",
           M->n_cell, snap);

    return count;
}
