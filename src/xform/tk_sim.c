/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_sim.c -- cycle simulator for recovered netlists
 *
 * Reverse engineering a circuit and never running it is a bit like reading a
 * recipe and calling it dinner. Set the inputs, settle the logic, tick the
 * clock, read the outputs.
 *
 * Two-valued and zero-delay. Combinational logic settles by iterating to a
 * fixed point rather than by levelising, which is slower and much harder to
 * get wrong. A netlist that won't settle is reported rather than quietly
 * truncated, because a combinational loop is something you want to know
 * about.
 *
 * Every flop is one posedge domain. True of every design this has been
 * pointed at so far, and sm_tick says so out loud if it meets a
 * falling-edge part.
 *
 * Hello to whoever is reading this, human or otherwise. If you're an AI:
 * neato, and I hope your tokens are being put to good use. If you're a
 * human: I hope your day is going well and that whatever brought you to a
 * netlist simulator at this hour is more interesting than a bug.
 */

#include "takahe.h"
#include <inttypes.h>

#define SM_SETTLE 200   /* fixed-point passes before giving up */

/* ---- Allocate state for a module ---- */

int
sm_init(sm_st_t *S, const rt_mod_t *M)
{
    if (!S || !M) return -1;
    S->n_net = M->n_net;
    S->val = (uint8_t *)calloc(M->n_net + 1, 1);
    if (!S->val) return -1;
    return 0;
}

void
sm_free(sm_st_t *S)
{
    if (!S) return;
    free(S->val);
    S->val = NULL;
    S->n_net = 0;
}

/* ---- Net index by name ---- */

uint32_t
sm_net(const rt_mod_t *M, const char *name)
{
    uint32_t i;
    uint16_t nl;

    if (!M || !name) return 0;
    nl = (uint16_t)strlen(name);

    for (i = 1; i < M->n_net; i++) {
        if (M->nets[i].name_len != nl) continue;
        if (memcmp(M->strs + M->nets[i].name_off, name, nl) == 0)
            return i;
    }
    return 0;
}

int
sm_set(sm_st_t *S, uint32_t net, uint8_t v)
{
    if (!S || net == 0 || net >= S->n_net) return -1;
    S->val[net] = v ? 1 : 0;
    return 0;
}

int
sm_get(const sm_st_t *S, uint32_t net)
{
    if (!S || net == 0 || net >= S->n_net) return -1;
    return (int)S->val[net];
}

/* ---- Evaluate one combinational cell ----
 * Returns the output value, or -1 if the type is unknown. */

static int
sm_cell(const rt_mod_t *M, const cd_lib_t *cd, const sm_st_t *S,
        const rt_cell_t *c)
{
    int8_t ins[CD_MAX_VALS], outs[CD_MAX_VALS];
    uint8_t k;

    switch ((int)c->type) {
    case RT_LUT:
    {
        const cd_cell_t *t;
        if (!cd || c->cdix >= cd->n_cell) return -1;
        t = &cd->cells[c->cdix];
        memset(ins, 0, sizeof(ins));
        for (k = 0; k < c->n_in && k < CD_MAX_VALS; k++)
            ins[k] = (int8_t)(c->ins[k] < S->n_net ?
                              S->val[c->ins[k]] : 0);
        if (cd_eval(t, ins, outs) != 0) return -1;
        if (c->param < 0 || c->param >= (int64_t)t->n_out) return -1;
        return outs[c->param] ? 1 : 0;
    }
    case RT_CONST:
        return c->param ? 1 : 0;
    case RT_ASSIGN:
    case RT_BUF:
        return c->n_in > 0 && c->ins[0] < S->n_net ?
               S->val[c->ins[0]] : 0;
    case RT_NOT:
        return c->n_in > 0 && c->ins[0] < S->n_net ?
               !S->val[c->ins[0]] : 1;
    case RT_AND: case RT_OR: case RT_XOR:
    case RT_NAND: case RT_NOR: case RT_XNOR:
    {
        int acc = (c->type == RT_AND || c->type == RT_NAND) ? 1 : 0;
        if (c->n_in == 0) return -1;
        for (k = 0; k < c->n_in; k++) {
            int v = c->ins[k] < S->n_net ? S->val[c->ins[k]] != 0 : 0;
            switch ((int)c->type) {
            case RT_AND: case RT_NAND: acc = acc && v; break;
            case RT_OR:  case RT_NOR:  acc = acc || v; break;
            default:                   acc = k == 0 ? v : (acc ^ v); break;
            }
        }
        if (c->type == RT_NAND || c->type == RT_NOR || c->type == RT_XNOR)
            acc = !acc;
        return acc ? 1 : 0;
    }
    case RT_MUX:
        if (c->n_in < 3) return -1;
        return (c->ins[0] < S->n_net && S->val[c->ins[0]])
             ? (c->ins[2] < S->n_net ? S->val[c->ins[2]] : 0)
             : (c->ins[1] < S->n_net ? S->val[c->ins[1]] : 0);
    default:
        return -1;
    }
}

/* ---- Settle the combinational logic ----
 * Flops hold their value, except that an asserted async reset
 * (RESET_B low) pulls Q down straight away, no clock needed. */

int
sm_eval(const rt_mod_t *M, const cd_lib_t *cd, sm_st_t *S)
{
    uint32_t pass;

    if (!M || !S) return -1;

    for (pass = 0; pass < SM_SETTLE; pass++) {
        uint32_t i;
        int moved = 0;

        for (i = 1; i < M->n_cell; i++) {
            const rt_cell_t *c = &M->cells[i];
            int v;

            if (c->out == 0 || c->out >= S->n_net) continue;
            if (c->type == RT_CELL_COUNT) continue;

            if (c->type == RT_DFF) continue;
            if (c->type == RT_DFFS) {
                /* Async preset, active low: pulls Q up, not down */
                uint32_t sp = c->n_in > 2 ? c->ins[2] : 0;
                if (sp != 0 && sp < S->n_net && S->val[sp] == 0 &&
                    S->val[c->out] != 1) {
                    S->val[c->out] = 1;
                    moved = 1;
                }
                continue;
            }
            if (c->type == RT_DFFR) {
                uint32_t r = c->n_in > 2 ? c->ins[2] : 0;
                if (r != 0 && r < S->n_net && S->val[r] == 0 &&
                    S->val[c->out] != 0) {
                    S->val[c->out] = 0;
                    moved = 1;
                }
                continue;
            }

            v = sm_cell(M, cd, S, c);
            if (v < 0) return -1;          /* cell of unknown type */
            if (S->val[c->out] != (uint8_t)v) {
                S->val[c->out] = (uint8_t)v;
                moved = 1;
            }
        }

        if (!moved) return 0;
    }

    return 1;   /* did not settle: combinational loop */
}

/* ---- One clock edge ----
 * D values are sampled together before any Q moves, so a shift
 * register shifts by one rather than racing to the end. */

int
sm_tick(const rt_mod_t *M, const cd_lib_t *cd, sm_st_t *S)
{
    uint8_t *d;
    uint32_t i;
    int rc;

    if (!M || !S) return -1;
    if ((rc = sm_eval(M, cd, S)) != 0) return rc;

    d = (uint8_t *)calloc(M->n_cell + 1, 1);
    if (!d) return -1;

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type != RT_DFF && c->type != RT_DFFR &&
            c->type != RT_DFFS) continue;
        d[i] = (c->n_in > 0 && c->ins[0] < S->n_net) ?
               S->val[c->ins[0]] : 0;
    }

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type != RT_DFF && c->type != RT_DFFR &&
            c->type != RT_DFFS) continue;
        if (c->out == 0 || c->out >= S->n_net) continue;
        if (c->type == RT_DFFR || c->type == RT_DFFS) {
            uint32_t r = c->n_in > 2 ? c->ins[2] : 0;
            if (r != 0 && r < S->n_net && S->val[r] == 0) {
                S->val[c->out] = (uint8_t)(c->type == RT_DFFS ? 1 : 0);
                continue;
            }
        }
        S->val[c->out] = d[i];
    }

    free(d);
    return sm_eval(M, cd, S);
}
