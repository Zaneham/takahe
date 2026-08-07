/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_sat.c -- CDCL SAT solver
 *
 * Conflict-driven clause learning with two watched literals, VSIDS
 * activity for branching, first-UIP learning, and Luby restarts. The
 * standard recipe, which has not changed much since Chaff in 2001 and
 * does not need to.
 *
 * Written in-tree because pulling in MiniSat would mean a C++ toolchain
 * and a submodule for something that is, in the end, about seven hundred
 * lines of bookkeeping.
 *
 * Literals are encoded as 2*var for the positive and 2*var+1 for the
 * negative, so the watch lists index straight in without a branch. The
 * DIMACS convention stays at the boundary, where it belongs.
 *
 * Learnt clause deletion is still missing. The database grows without
 * bound, so very long runs will exhaust the pool rather than slow down
 * gracefully. Fine for what it is pointed at today, and the next thing
 * to add.
 */

#include "takahe.h"
#include <inttypes.h>

#define SA_UNDEF  0
#define SA_TRUE   1
#define SA_FALSE  2

#define LIT(l)    ((l) > 0 ? (((uint32_t)(l)) << 1) \
                           : ((((uint32_t)(-(l))) << 1) | 1u))
#define VAR(x)    ((x) >> 1)
#define SIGN(x)   ((x) & 1u)
#define NEG(x)    ((x) ^ 1u)

typedef struct {
    uint32_t *lits;      /* literal pool                        */
    uint32_t *start;     /* clause -> first literal             */
    uint32_t  n_cls, max_cls;
    uint32_t  n_lit, max_lit;

    uint32_t  n_var;
    uint8_t  *val;       /* per variable: UNDEF / TRUE / FALSE   */
    int32_t  *level;     /* decision level of each assignment    */
    uint32_t *reason;    /* clause that forced it, or 0          */
    double   *act;       /* VSIDS activity                       */
    uint8_t  *seen;

    uint32_t *trail;
    uint32_t  n_trail, qhead;
    uint32_t *lim;       /* trail index where each level began   */
    int32_t   dl;

    uint32_t *wl;        /* watch lists, flattened               */
    uint32_t *wn, *wcap, *woff;

    /* Decision heap. A linear scan over activity is fine at a few
     * hundred variables and ruinous at eighty thousand, which is
     * exactly the size that matters. Binary max-heap, and lazy, so a
     * variable may sit in the heap while assigned and gets skipped on
     * the way out. */
    uint32_t *heap;      /* heap of variable indices            */
    int32_t  *hpos;      /* variable -> index in heap, -1 if out */
    uint32_t  n_heap;

    uint8_t  *phase;     /* last value each variable took       */

    uint64_t  conf;
    double    vinc;
} sa_t;

/* ---- Decision heap ---- */

static void
sa_hup(sa_t *S, uint32_t i)
{
    uint32_t v = S->heap[i];
    while (i > 0) {
        uint32_t p = (i - 1) >> 1;
        if (S->act[S->heap[p]] >= S->act[v]) break;
        S->heap[i] = S->heap[p];
        S->hpos[S->heap[i]] = (int32_t)i;
        i = p;
    }
    S->heap[i] = v;
    S->hpos[v] = (int32_t)i;
}

static void
sa_hdn(sa_t *S, uint32_t i)
{
    uint32_t v = S->heap[i];
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= S->n_heap) break;
        if (c + 1 < S->n_heap && S->act[S->heap[c + 1]] > S->act[S->heap[c]])
            c++;
        if (S->act[S->heap[c]] <= S->act[v]) break;
        S->heap[i] = S->heap[c];
        S->hpos[S->heap[i]] = (int32_t)i;
        i = c;
    }
    S->heap[i] = v;
    S->hpos[v] = (int32_t)i;
}

static void
sa_hins(sa_t *S, uint32_t v)
{
    if (S->hpos[v] >= 0) return;
    S->heap[S->n_heap] = v;
    S->hpos[v] = (int32_t)S->n_heap;
    S->n_heap++;
    sa_hup(S, S->n_heap - 1);
}

static uint32_t
sa_hpop(sa_t *S)
{
    uint32_t top;
    if (S->n_heap == 0) return 0;
    top = S->heap[0];
    S->hpos[top] = -1;
    S->n_heap--;
    if (S->n_heap > 0) {
        S->heap[0] = S->heap[S->n_heap];
        S->hpos[S->heap[0]] = 0;
        sa_hdn(S, 0);
    }
    return top;
}


/* ---- Watch lists ----
 * A vector per literal, grown by doubling into one flat pool. Rebuilding
 * the pool on growth is cheaper than a linked list and much friendlier to
 * the cache, which is the whole reason watched literals exist. */

static int
sa_wpush(sa_t *S, uint32_t lit, uint32_t cls)
{
    if (S->wn[lit] >= S->wcap[lit]) {
        uint32_t ncap = S->wcap[lit] ? S->wcap[lit] * 2 : 4;
        /* Grow by relocating this list to the end of the pool */
        {
            uint32_t need = S->woff[2 * S->n_var + 2] + ncap;
            uint32_t *pool = (uint32_t *)realloc(S->wl,
                                (size_t)need * sizeof(uint32_t));
            uint32_t i, base;
            if (!pool) return -1;
            S->wl = pool;
            base = S->woff[2 * S->n_var + 2];
            for (i = 0; i < S->wn[lit]; i++)
                S->wl[base + i] = S->wl[S->woff[lit] + i];
            S->woff[lit] = base;
            S->wcap[lit] = ncap;
            S->woff[2 * S->n_var + 2] = base + ncap;
        }
    }
    S->wl[S->woff[lit] + S->wn[lit]] = cls;
    S->wn[lit]++;
    return 0;
}

static uint8_t
sa_lval(const sa_t *S, uint32_t lit)
{
    uint8_t v = S->val[VAR(lit)];
    if (v == SA_UNDEF) return SA_UNDEF;
    if (!SIGN(lit)) return v;
    return v == SA_TRUE ? SA_FALSE : SA_TRUE;
}

static void
sa_assign(sa_t *S, uint32_t lit, uint32_t reason)
{
    uint32_t v = VAR(lit);
    S->val[v] = SIGN(lit) ? SA_FALSE : SA_TRUE;
    S->phase[v] = S->val[v];        /* remember for the next decision */
    S->level[v] = S->dl;
    S->reason[v] = reason;
    S->trail[S->n_trail++] = lit;
}

/* ---- Propagate ---- */

static uint32_t
sa_prop(sa_t *S)
{
    while (S->qhead < S->n_trail) {
        uint32_t p = S->trail[S->qhead++];
        uint32_t fl = NEG(p);           /* clauses watching this go false */
        uint32_t i = 0, keep = 0;
        uint32_t n = S->wn[fl];
        uint32_t base = S->woff[fl];

        while (i < n) {
            uint32_t c = S->wl[base + i++];
            uint32_t st = S->start[c], en = S->start[c + 1];
            uint32_t *L = &S->lits[st];
            uint32_t len = en - st, j;
            uint32_t other;

            /* Keep the watched pair in slots 0 and 1 */
            if (L[0] == fl) { L[0] = L[1]; L[1] = fl; }
            other = L[0];

            if (sa_lval(S, other) == SA_TRUE) {
                S->wl[base + keep++] = c;
                continue;
            }
            for (j = 2; j < len; j++) {
                if (sa_lval(S, L[j]) != SA_FALSE) {
                    L[1] = L[j]; L[j] = fl;
                    if (sa_wpush(S, L[1], c) != 0) return 0;
                    goto next;
                }
            }
            S->wl[base + keep++] = c;
            if (sa_lval(S, other) == SA_FALSE) {
                /* Conflict: keep the rest of the list intact */
                while (i < n) S->wl[base + keep++] = S->wl[base + i++];
                S->wn[fl] = keep;
                return c;
            }
            sa_assign(S, other, c);
        next: ;
        }
        S->wn[fl] = keep;
    }
    return 0;
}

/* ---- Learn: first UIP ---- */

static uint32_t
sa_learn(sa_t *S, uint32_t conf, uint32_t *out, uint32_t *nout,
         int32_t *btlevel)
{
    uint32_t cnt = 0, idx = S->n_trail, p = 0, n = 0;
    uint32_t c = conf;

    out[n++] = 0;   /* placeholder for the asserting literal */
    do {
        uint32_t st = S->start[c], en = S->start[c + 1], j;
        for (j = (p == 0 ? 0 : 1); j < en - st; j++) {
            uint32_t q = S->lits[st + j];
            uint32_t v = VAR(q);
            if (S->seen[v] || S->level[v] == 0) continue;
            S->seen[v] = 1;
            S->act[v] += S->vinc;
            if (S->hpos[v] >= 0) sa_hup(S, (uint32_t)S->hpos[v]);
            if (S->level[v] >= S->dl) cnt++;
            else out[n++] = q;
        }
        do { p = S->trail[--idx]; } while (!S->seen[VAR(p)]);
        c = S->reason[VAR(p)];
        S->seen[VAR(p)] = 0;
        cnt--;
    } while (cnt > 0);

    out[0] = NEG(p);

    /* Backtrack to the second-highest level in the clause */
    *btlevel = 0;
    {
        uint32_t i, best = 1;
        for (i = 1; i < n; i++)
            if (S->level[VAR(out[i])] > S->level[VAR(out[best])]) best = i;
        if (n > 1) {
            uint32_t t = out[1]; out[1] = out[best]; out[best] = t;
            *btlevel = S->level[VAR(out[1])];
        }
    }
    for (idx = 0; idx < n; idx++) S->seen[VAR(out[idx])] = 0;
    *nout = n;
    return 0;
}

static void
sa_cancel(sa_t *S, int32_t lvl)
{
    uint32_t i;
    if (S->dl <= lvl) return;
    for (i = S->n_trail; i > S->lim[lvl]; i--) {
        uint32_t v = VAR(S->trail[i - 1]);
        S->val[v] = SA_UNDEF;
        S->reason[v] = 0;
        sa_hins(S, v);
    }
    S->n_trail = S->lim[lvl];
    S->qhead = S->n_trail;
    S->dl = lvl;
}

static uint32_t
sa_addcl(sa_t *S, const uint32_t *lits, uint32_t n)
{
    uint32_t c, i;

    if (S->n_cls >= S->max_cls || S->n_lit + n > S->max_lit) return 0;
    c = S->n_cls;
    for (i = 0; i < n; i++) S->lits[S->n_lit + i] = lits[i];
    S->n_lit += n;
    S->n_cls++;
    S->start[S->n_cls] = S->n_lit;
    if (n >= 2) {
        if (sa_wpush(S, lits[0], c) != 0) return 0;
        if (sa_wpush(S, lits[1], c) != 0) return 0;
    }
    return c + 1;
}

/* Luby sequence, for restart intervals */
static uint32_t
sa_luby(uint32_t i)
{
    uint32_t k;
    for (k = 1; k < 32; k++)
        if (i == (1u << k) - 1) return 1u << (k - 1);
    for (k = 1;; k++)
        if ((1u << (k - 1)) <= i && i < (1u << k) - 1)
            return sa_luby(i - (1u << (k - 1)) + 1);
}

/* ---- Public ---- */

int
sa_solve(const cn_t *C, uint8_t *model, uint64_t max_conf)
{
    sa_t S;
    uint32_t i, j, nv, nl, *learnt;
    int rc = -1;
    uint32_t restarts = 0, until;

    if (!C || C->n_var == 0) return -1;
    memset(&S, 0, sizeof(S));
    nv = C->n_var;
    nl = 2 * nv + 4;

    S.max_cls = C->n_cls + (uint32_t)(max_conf > 400000 ? 400000 : max_conf) + 16;
    S.max_lit = C->n_lit + S.max_cls * 8 + 64;
    S.n_var = nv;
    S.lits   = (uint32_t *)malloc((size_t)S.max_lit * sizeof(uint32_t));
    S.start  = (uint32_t *)malloc(((size_t)S.max_cls + 2) * sizeof(uint32_t));
    S.val    = (uint8_t  *)calloc(nv + 2, 1);
    S.level  = (int32_t  *)calloc(nv + 2, sizeof(int32_t));
    S.reason = (uint32_t *)calloc(nv + 2, sizeof(uint32_t));
    S.act    = (double   *)calloc(nv + 2, sizeof(double));
    S.seen   = (uint8_t  *)calloc(nv + 2, 1);
    S.trail  = (uint32_t *)malloc((size_t)(nv + 2) * sizeof(uint32_t));
    S.lim    = (uint32_t *)calloc(nv + 2, sizeof(uint32_t));
    S.wn     = (uint32_t *)calloc(nl + 2, sizeof(uint32_t));
    S.wcap   = (uint32_t *)calloc(nl + 2, sizeof(uint32_t));
    S.woff   = (uint32_t *)calloc(nl + 4, sizeof(uint32_t));
    S.wl     = (uint32_t *)malloc(sizeof(uint32_t) * 16);
    S.heap   = (uint32_t *)malloc((size_t)(nv + 2) * sizeof(uint32_t));
    S.hpos   = (int32_t  *)malloc((size_t)(nv + 2) * sizeof(int32_t));
    S.phase  = (uint8_t  *)calloc(nv + 2, 1);
    learnt   = (uint32_t *)malloc((size_t)(nv + 4) * sizeof(uint32_t));

    if (!S.lits || !S.start || !S.val || !S.level || !S.reason || !S.act ||
        !S.seen || !S.trail || !S.lim || !S.wn || !S.wcap || !S.woff ||
        !S.wl || !learnt || !S.heap || !S.hpos || !S.phase) {
        rc = -1; goto done;
    }

    /* Every variable starts in the heap, all at zero activity, so the
     * first decisions come out in index order until conflicts start
     * saying something more useful. */
    for (i = 1; i <= nv; i++) S.hpos[i] = -1;
    S.hpos[0] = -1;
    for (i = 1; i <= nv; i++) sa_hins(&S, i);

    S.start[0] = 0;
    S.vinc = 1.0;
    /* woff[2*n_var+2] doubles as the bump cursor for the watch pool.
     * calloc already zeroed it; named here so the next reader doesn't
     * have to work out why that slot is special. */
    S.woff[2 * nv + 2] = 0;

    /* Load the formula */
    for (i = 0; i < C->n_cls; i++) {
        uint32_t buf[512], n = 0;
        int sat = 0;
        for (j = C->cls[i]; j < C->cls[i + 1] && n < 512; j++) {
            int32_t l = C->lits[j];
            if (l == 0) continue;
            buf[n++] = LIT(l);
        }
        if (n == 0) { rc = 0; goto done; }
        if (n == 1) {
            if (sa_lval(&S, buf[0]) == SA_FALSE) { rc = 0; goto done; }
            if (sa_lval(&S, buf[0]) == SA_UNDEF) sa_assign(&S, buf[0], 0);
            continue;
        }
        if (sat) continue;
        if (sa_addcl(&S, buf, n) == 0) { rc = -1; goto done; }
    }

    if (sa_prop(&S) != 0) { rc = 0; goto done; }

    until = 100;
    for (;;) {
        uint32_t conf = sa_prop(&S);

        if (conf) {
            uint32_t n = 0;
            int32_t bt = 0;
            S.conf++;
            if (S.dl == 0) { rc = 0; goto done; }
            if (S.conf > max_conf) { rc = -1; goto done; }
            sa_learn(&S, conf, learnt, &n, &bt);
            sa_cancel(&S, bt);
            {
                uint32_t c = sa_addcl(&S, learnt, n);
                if (n == 1) sa_assign(&S, learnt[0], 0);
                else if (c) sa_assign(&S, learnt[0], c - 1);
                else { rc = -1; goto done; }
            }
            S.vinc *= 1.05;
            if (S.vinc > 1e100) {
                for (i = 1; i <= nv; i++) S.act[i] *= 1e-100;
                S.vinc *= 1e-100;
            }
            continue;
        }

        if (S.conf >= until) {
            restarts++;
            until = (uint32_t)(S.conf + 100 * sa_luby(restarts));
            sa_cancel(&S, 0);
            continue;
        }

        /* Branch on the unassigned variable with the highest activity */
        {
            uint32_t best = 0;
            /* Pop until an unassigned variable turns up. The heap is
             * lazy, so assigned variables can still be sitting in it. */
            for (;;) {
                best = sa_hpop(&S);
                if (best == 0) break;
                if (S.val[best] == SA_UNDEF) break;
            }
            if (best == 0) { rc = 1; goto done; }
            S.lim[S.dl] = S.n_trail;
            S.dl++;
            S.lim[S.dl] = S.n_trail;
            /* Phase saving: go back to whatever this variable last was.
             * Restarts throw away the search tree, not what the search
             * learned about which way each variable wants to sit. */
            sa_assign(&S, (best << 1) |
                      (S.phase[best] == SA_TRUE ? 0u : 1u), 0);
        }
    }

done:
    if (rc == 1 && model)
        for (i = 1; i <= nv; i++)
            model[i] = (uint8_t)(S.val[i] == SA_TRUE ? 1 : 0);

    free(S.lits); free(S.start); free(S.val); free(S.level);
    free(S.reason); free(S.act); free(S.seen); free(S.trail);
    free(S.lim); free(S.wn); free(S.wcap); free(S.woff); free(S.wl);
    free(S.heap); free(S.hpos); free(S.phase);
    free(learnt);
    return rc;
}
