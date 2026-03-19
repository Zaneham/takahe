/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_jrn.c -- CICS-style transaction journal for Takahe RTL
 *
 * Every mutation to the netlist is journaled. If a pass fails
 * or makes things worse, discard the journal and revert. No
 * half-optimised netlists. No corrupted state. No "well it
 * was working before I ran Espresso."
 *
 * The pattern comes from IBM's Customer Information Control
 * System (CICS), first deployed in 1968 on System/360. CICS
 * processes millions of transactions per second for banks,
 * airlines, and insurers. Every transaction is journaled so
 * that a failure at any point can be cleanly rolled back to
 * the last consistent state.
 *
 * We do the same for synthesis passes. The "transaction" is
 * an optimisation pass (cprop, Espresso, timing-driven
 * resize). The "journal" records every cell added, deleted,
 * or modified. The "rollback" undoes them in reverse order.
 *
 * The result: you can run aggressive optimisations knowing
 * that if they make things worse, the netlist reverts cleanly.
 * Like a bank transaction: either the whole transfer completes,
 * or none of it does. Your netlist never ends up with money
 * in neither account.
 *
 * References (APA 7th):
 *
 * IBM Corporation. (1977). CICS/VS System Programmer's
 *   Reference Manual (SC33-0068). IBM Systems Library.
 *
 * IBM Corporation. (2014). CICS Transaction Server for z/OS:
 *   Recovery and Restart Guide (SC34-7012). IBM Knowledge
 *   Center.
 *
 * Gray, J., & Reuter, A. (1993). Transaction Processing:
 *   Concepts and Techniques. Morgan Kaufmann.
 *   https://doi.org/10.1016/C2009-0-27825-8
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

/* Single global journal — one transaction at a time.
 * CICS supports nested transactions. We don't need to:
 * synthesis passes don't nest. Keep it simple. */

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
    /* Journal discarded. Changes are permanent.
     * Like closing a bank transaction: the money moved. */
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
