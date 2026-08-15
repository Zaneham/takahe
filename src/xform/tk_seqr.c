/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_seqr.c -- sequential structure recovery
 *
 * A recovered netlist gives you ninety-odd flops and no hint as to which are
 * a shift register, which are a counter, and which are the two bits of state
 * that actually decide anything.
 *
 * So walk backwards from each flop's D through the combinational logic until
 * you hit another flop, and see which flops you land on. Fed by exactly one
 * other flop is a shift stage. Fed only by itself is holding, which is what
 * the head of an enabled shift register looks like. Fed by a crowd is doing
 * something worth reading properly.
 *
 * No solver anywhere near this. It's a graph walk, and the structure was in
 * the netlist the whole time.
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Is this cell combinational for cone-walking purposes? ---- */

static int
sq_comb(rt_ctype_t t)
{
    switch ((int)t) {
    case RT_DFF:
    case RT_DFFR:
    case RT_DFFS:
    case RT_DLAT:
        return 0;
    default:
        return 1;
    }
}

/* ---- Backward cone from one net, collecting flop sources ----
 * Iterative worklist, because recursion is off the menu and a
 * fan-in cone is exactly the shape that would blow a stack. */

static void
sq_cone(const rt_mod_t *M, uint32_t start, const uint32_t *q2ff,
        uint32_t *seen, uint32_t stamp, sq_ff_t *ff, uint32_t self)
{
    uint32_t stack[SQ_MAX_STK];
    uint32_t sp = 0;

    if (start == 0 || start >= M->n_net) return;

    stack[sp++] = start;
    {
    KA_GUARD(g, SQ_MAX_STK * 64);
    while (sp > 0 && g--) {
        uint32_t ni = stack[--sp];
        const rt_cell_t *c;
        uint32_t drv;
        uint8_t k;

        if (ni == 0 || ni >= M->n_net) continue;
        if (seen[ni] == stamp) continue;
        seen[ni] = stamp;

        /* A flop output ends the walk. That's the whole point. */
        if (q2ff[ni] != 0) {
            uint32_t src = q2ff[ni] - 1;
            if (src == self) {
                ff->self = 1;
            } else if (ff->nsrc < SQ_MAX_SRC) {
                uint32_t j;
                for (j = 0; j < ff->nsrc; j++)
                    if (ff->src[j] == src) break;
                if (j == ff->nsrc) ff->src[ff->nsrc++] = src;
            } else {
                ff->nsrc++;   /* count past the cap, don't store */
            }
            continue;
        }

        drv = M->nets[ni].driver;
        if (drv == 0 || drv >= M->n_cell) continue;
        c = &M->cells[drv];
        if (!sq_comb(c->type)) continue;

        for (k = 0; k < c->n_in; k++)
            if (sp < SQ_MAX_STK) stack[sp++] = c->ins[k];
    }
    }
}

/* ---- Public: recover sequential structure ---- */

int
sq_scan(const rt_mod_t *M, sq_res_t *R)
{
    uint32_t *q2ff, *seen;
    uint32_t i, stamp = 0;

    if (!M || !R) return -1;

    memset(R, 0, sizeof(*R));

    q2ff = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    seen = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    if (!q2ff || !seen) { free(q2ff); free(seen); return -1; }

    /* Collect flops, index them by their Q net */
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type != RT_DFF && c->type != RT_DFFR &&
            c->type != RT_DFFS) continue;
        if (R->n_ff >= SQ_MAX_FF) break;
        R->ff[R->n_ff].cell = i;
        R->ff[R->n_ff].q    = c->out;
        R->ff[R->n_ff].d    = c->n_in > 0 ? c->ins[0] : 0;
        R->ff[R->n_ff].clk  = c->n_in > 1 ? c->ins[1] : 0;
        R->ff[R->n_ff].rst  = c->n_in > 2 ? c->ins[2] : 0;
        R->ff[R->n_ff].chain = -1;
        R->ff[R->n_ff].grp   = -1;
        if (c->out > 0 && c->out < M->n_net)
            q2ff[c->out] = R->n_ff + 1;
        R->n_ff++;
    }

    /* Walk each D cone back to whichever flops feed it */
    for (i = 0; i < R->n_ff; i++) {
        stamp++;
        sq_cone(M, R->ff[i].d, q2ff, seen, stamp, &R->ff[i], i);

        if (R->ff[i].nsrc == 0)
            R->ff[i].kind = (uint8_t)SQ_HOLD;
        else if (R->ff[i].nsrc == 1)
            R->ff[i].kind = (uint8_t)SQ_SHIFT;
        else
            R->ff[i].kind = (uint8_t)SQ_FSM;
    }

    /* Group flops that share control signals. DANA's observation:
     * flops clocked and reset together are usually one register.
     * Note this only sees control that reaches a pin. An enable
     * built out of a mux in the D path is datapath, not control,
     * and won't split a group here. */
    for (i = 0; i < R->n_ff; i++) {
        uint32_t j;

        if (R->ff[i].grp >= 0) continue;
        R->ff[i].grp = (int32_t)R->n_grp;
        R->grpff[R->n_grp] = i;
        R->grplen[R->n_grp] = 1;

        for (j = i + 1; j < R->n_ff; j++) {
            if (R->ff[j].grp >= 0) continue;
            if (R->ff[j].clk != R->ff[i].clk) continue;
            if (R->ff[j].rst != R->ff[i].rst) continue;
            R->ff[j].grp = (int32_t)R->n_grp;
            R->grplen[R->n_grp]++;
        }
        R->n_grp++;
    }

    /* Shift graph degrees. A chain only survives where each link
     * is unambiguous: one predecessor, one successor. Fan-out
     * means it's a distribution point, not a shift stage. */
    for (i = 0; i < R->n_ff; i++) {
        if (R->ff[i].kind != SQ_SHIFT) continue;
        R->ff[i].indeg++;
        R->ff[R->ff[i].src[0]].outdeg++;
    }

    /* Walk chains from each head */
    for (i = 0; i < R->n_ff; i++) {
        uint32_t cur, pos = 0;

        if (R->ff[i].chain >= 0) continue;
        if (R->ff[i].indeg != 0) continue;    /* not a head */
        if (R->ff[i].outdeg != 1) continue;   /* leads nowhere */

        cur = i;
        {
        KA_GUARD(g, SQ_MAX_FF + 1);
        while (g--) {
            uint32_t nx, found = 0, j;

            R->ff[cur].chain = (int32_t)R->n_chain;
            R->ff[cur].pos = pos++;

            if (R->ff[cur].outdeg != 1) break;

            nx = 0;
            for (j = 0; j < R->n_ff; j++) {
                if (R->ff[j].kind != SQ_SHIFT) continue;
                if (R->ff[j].src[0] != cur) continue;
                nx = j; found = 1; break;
            }
            if (!found) break;
            if (R->ff[nx].indeg != 1) break;
            if (R->ff[nx].chain >= 0) break;   /* ring, stop */
            cur = nx;
        }
        }

        R->chlen[R->n_chain] = pos;
        R->chhead[R->n_chain] = i;
        R->n_chain++;
    }

    free(q2ff);
    free(seen);
    return 0;
}

/* ---- Public: human-readable summary ---- */

void
sq_rep(const rt_mod_t *M, const sq_res_t *R)
{
    uint32_t i, hold = 0, shift = 0, fsm = 0;

    for (i = 0; i < R->n_ff; i++) {
        if (R->ff[i].kind == SQ_HOLD)  hold++;
        else if (R->ff[i].kind == SQ_SHIFT) shift++;
        else fsm++;
    }

    printf("takahe: seq: %u flops (%u held, %u shift, %u multi-source)\n",
           R->n_ff, hold, shift, fsm);

    for (i = 0; i < R->n_chain; i++) {
        uint32_t h = R->chhead[i];
        const rt_net_t *n = &M->nets[R->ff[h].q];
        printf("  chain %u: %u stages, head net '%.*s'\n",
               i, R->chlen[i], (int)n->name_len, M->strs + n->name_off);
    }

    for (i = 0; i < R->n_grp; i++) {
        uint32_t f = R->grpff[i];
        const rt_net_t *ck = R->ff[f].clk < M->n_net ?
                             &M->nets[R->ff[f].clk] : NULL;
        printf("  group %u: %u flops, clock '%.*s'\n",
               i, R->grplen[i],
               ck ? (int)ck->name_len : 1,
               ck ? M->strs + ck->name_off : "?");
    }
}
