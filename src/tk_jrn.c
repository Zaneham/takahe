/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_jrn.c -- transaction journal for the RTL netlist
 *
 * Every mutation gets journaled so a pass that makes things worse can be
 * rolled back cleanly. Straight out of CICS, which has done this since 1968.
 *
 * See docs/references.md.
 */

#include "takahe.h"

/* ---- Journal entry: one mutation ---- */

typedef enum {
    JR_CELL_ADD = 0,   /* cell was created at index ci     */
    JR_CELL_DEL,       /* cell at ci was killed (saved type) */
    JR_CELL_MOD,       /* cell at ci was modified (saved)   */
    JR_NET_ADD,        /* net was created at index ni       */
    JR_BIND_MOD        /* binding table entry changed       */
} jr_op_t;

typedef struct {
    jr_op_t    op;
    uint32_t   idx;       /* cell or net index                */
    rt_cell_t  cell_sav;  /* saved cell state (for rollback)  */
    mp_bind_t  bind_sav;  /* saved binding (for tdopt)        */
    uint8_t    bind_ct;   /* which rt_ctype_t was modified    */
} jr_ent_t;

/* ---- Journal ring buffer ---- */

#define JR_MAX  16384   /* max entries per transaction */

typedef struct {
    jr_ent_t  ents[JR_MAX];
    uint32_t  n;
    uint32_t  snap_net;   /* net count at snapshot       */
    uint32_t  snap_cell;  /* cell count at snapshot      */
    uint32_t  snap_str;   /* string pool len at snapshot */
    int       active;     /* 1 = transaction in progress */
} jr_ctx_t;

/* Single global journal, one transaction at a time. CICS nests them.
 * Synthesis passes don't, so this doesn't either. */

static jr_ctx_t JR;

/* ---- Public: begin a transaction ---- */

void
jr_begin(const rt_mod_t *M)
{
    memset(&JR, 0, sizeof(JR));
    if (M) {
        JR.snap_net  = M->n_net;
        JR.snap_cell = M->n_cell;
        JR.snap_str  = M->str_len;
    }
    JR.active = 1;
}

/* ---- Public: record a cell addition ---- */

void
jr_acell(uint32_t ci)
{
    if (!JR.active || JR.n >= JR_MAX) return;
    JR.ents[JR.n].op  = JR_CELL_ADD;
    JR.ents[JR.n].idx = ci;
    JR.n++;
}

/* ---- Public: record a cell deletion (save state) ---- */

void
jr_dcell(const rt_mod_t *M, uint32_t ci)
{
    if (!JR.active || JR.n >= JR_MAX) return;
    if (ci == 0 || ci >= M->n_cell) return;
    JR.ents[JR.n].op       = JR_CELL_DEL;
    JR.ents[JR.n].idx      = ci;
    JR.ents[JR.n].cell_sav = M->cells[ci];
    JR.n++;
}

/* ---- Public: record a cell modification (save before) ---- */

void
jr_mcell(const rt_mod_t *M, uint32_t ci)
{
    if (!JR.active || JR.n >= JR_MAX) return;
    if (ci == 0 || ci >= M->n_cell) return;
    JR.ents[JR.n].op       = JR_CELL_MOD;
    JR.ents[JR.n].idx      = ci;
    JR.ents[JR.n].cell_sav = M->cells[ci];
    JR.n++;
}

/* ---- Public: record a binding change ---- */

void
jr_mbind(const mp_bind_t *tbl, uint8_t ct)
{
    if (!JR.active || JR.n >= JR_MAX) return;
    if (ct >= RT_CELL_COUNT) return;
    JR.ents[JR.n].op       = JR_BIND_MOD;
    JR.ents[JR.n].bind_ct  = ct;
    JR.ents[JR.n].bind_sav = tbl[ct];
    JR.n++;
}

/* ---- Public: commit — accept all changes ---- */

void
jr_commit(void)
{
    JR.active = 0;
    JR.n = 0;
    /* Journal discarded, so the changes are permanent. */
}

/* ---- Public: rollback — undo all changes ---- */

void
jr_rback(rt_mod_t *M, mp_bind_t *tbl)
{
    uint32_t i;

    if (!JR.active || !M) { JR.active = 0; return; }

    /* Replay journal in reverse. Each entry undoes one
     * mutation. Like rewinding a tape, except the tape
     * is your netlist and the music was Espresso. */
    for (i = JR.n; i > 0; i--) {
        jr_ent_t *e = &JR.ents[i - 1];

        switch (e->op) {
        case JR_CELL_ADD:
            /* Cell was added — kill it */
            if (e->idx > 0 && e->idx < M->n_cell)
                M->cells[e->idx].type = RT_CELL_COUNT;
            break;

        case JR_CELL_DEL:
            /* Cell was killed — restore it */
            if (e->idx > 0 && e->idx < M->n_cell)
                M->cells[e->idx] = e->cell_sav;
            break;

        case JR_CELL_MOD:
            /* Cell was modified — restore original */
            if (e->idx > 0 && e->idx < M->n_cell)
                M->cells[e->idx] = e->cell_sav;
            break;

        case JR_BIND_MOD:
            /* Binding was changed — restore */
            if (tbl && e->bind_ct < RT_CELL_COUNT)
                tbl[e->bind_ct] = e->bind_sav;
            break;

        case JR_NET_ADD:
            /* Net was added — can't easily undo (would leave
             * dangling references). Just note it. Rollback of
             * net additions is handled by restoring snap counts. */
            break;

        default:
            break;
        }
    }

    /* Restore pool counts to snapshot.
     * Nets/cells added after snapshot are now garbage
     * (their cells were killed above). The pool counts
     * revert so new allocations overwrite them. */
    M->n_net   = JR.snap_net;
    M->n_cell  = JR.snap_cell;
    M->str_len = JR.snap_str;

    JR.active = 0;
    JR.n = 0;

    printf("takahe: journal: rolled back %u mutations\n",
           (unsigned)JR.n);
}

/* ---- Public: is a transaction active? ---- */

int
jr_active(void)
{
    return JR.active;
}

/* ---- Public: how many entries in current transaction? ---- */

uint32_t
jr_count(void)
{
    return JR.n;
}
