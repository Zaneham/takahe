/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_fi.c -- does a single upset actually reach anything?
 *
 * A TMR pass triplicates flops and votes on them, and then nothing checks
 * that the result still works. The classic way it fails is the optimiser
 * looking at three identical logic cones, sensibly concluding they are the
 * same cone, and merging them. Area goes up threefold, immunity stays where
 * it was, and the netlist looks fine.
 *
 * So ask properly. Two copies of the combinational logic against the same
 * inputs, an XOR spliced into one node of one copy, exactly one splice
 * constrained live, and SAT asked whether any input makes the copies
 * disagree. Unsatisfiable means every single upset is masked, which is a
 * proof rather than a fault campaign that got bored and stopped.
 *
 * Satisfiable is more useful still, because the model names the node that
 * got through. Block it, ask again, and the answer arrives as a list.
 *
 * Cut at the flops in the usual way, every flop output free and every flop D
 * watched, so the whole question stays combinational and nothing needs
 * unrolling.
 */

#include "takahe.h"
#include <inttypes.h>

#define FI_CONF 2000000u   /* conflict budget for one solve */
#define FI_SHOW 32u        /* unprotected sites listed before eliding */

static int
fi_isff(rt_ctype_t t)
{
    return t == RT_DFF || t == RT_DFFR || t == RT_DFFS;
}

/* ---- Working copy ----
 * Nothing here means anything until wide operators come apart into single
 * bits, and mp_bblst does that by rewriting the module. An analysis pass
 * has no business handing back a design the caller did not ask for, so the
 * blasting happens to a clone that gets thrown away afterwards.
 *
 * Room is budgeted from the widths actually present. A ripple-carry adder
 * runs to a handful of gates a bit once the carries are counted, and a flat
 * multiplier would ask for gigabytes on anything large. */

static rt_mod_t *
fi_clone(const rt_mod_t *M)
{
    rt_mod_t *W;
    uint32_t i, nc, nn;

    nc = M->n_cell + 16;
    nn = M->n_net + 16;
    for (i = 1; i < M->n_cell; i++) {
        uint32_t w = M->cells[i].width ? M->cells[i].width : 1;
        if (w > 64) w = 64;
        nc += 8 * w;
        nn += 8 * w;
    }

    W = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    if (!W) return NULL;
    if (rt_init(W, nn, nc) != 0) { free(W); return NULL; }

    memcpy(W->nets,  M->nets,  (size_t)M->n_net * sizeof(rt_net_t));
    memcpy(W->cells, M->cells, (size_t)M->n_cell * sizeof(rt_cell_t));
    if (M->str_len <= W->str_max) memcpy(W->strs, M->strs, M->str_len);
    memcpy(W->mod_name, M->mod_name, sizeof(W->mod_name));
    W->n_net     = M->n_net;
    W->n_cell    = M->n_cell;
    W->str_len   = M->str_len;
    W->top_net_lo = M->top_net_lo;
    return W;
}

/* ---- y = a XOR b ---- */

static int
fi_xor(cn_t *C, uint32_t a, uint32_t b, uint32_t y)
{
    uint32_t in2[2];

    if (a == 0 || b == 0 || y == 0) return -1;
    in2[0] = a; in2[1] = b;
    return cn_gate(C, 0x6u, in2, 2, y);
}

/* ---- Force two variables to agree ---- */

static int
fi_tie(cn_t *C, uint32_t a, uint32_t b)
{
    int32_t c1[2], c2[2];

    if (a == 0 || b == 0) return -1;
    c1[0] = -(int32_t)a; c1[1] =  (int32_t)b;
    c2[0] =  (int32_t)a; c2[1] = -(int32_t)b;
    if (cn_add(C, c1, 2) != 0) return -1;
    return cn_add(C, c2, 2);
}

/* ---- Where an upset could land ----
 * Flop outputs always, since a stored upset is the thing TMR exists to
 * survive. Gate outputs only when asked, because a glitch has to reach a
 * flop to matter and including them buries the answer in noise. */

static void
fi_name(const rt_mod_t *M, uint32_t net, char *buf, size_t n)
{
    uint16_t l;

    if (net == 0 || net >= M->n_net) { snprintf(buf, n, "?"); return; }
    l = M->nets[net].name_len;
    if (l == 0) { snprintf(buf, n, "n%u", net); return; }
    if ((size_t)l >= n) l = (uint16_t)(n - 1);
    memcpy(buf, M->strs + M->nets[net].name_off, l);
    buf[l] = '\0';
}

static void
fi_site(const rt_mod_t *M, int comb, fi_res_t *R)
{
    uint32_t i;

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        fi_site_t *s;
        int seq;

        if (c->type == RT_CELL_COUNT) continue;
        if (c->out == 0 || c->out >= M->n_net) continue;

        seq = fi_isff(c->type);
        if (!seq && !comb) continue;

        if (R->n_site >= FI_MAXFLT) { R->trunc = 1; return; }
        s = &R->site[R->n_site];
        s->net = c->out;
        s->sel = 0;
        s->seq = (uint8_t)seq;
        fi_name(M, c->out, s->name, sizeof(s->name));
        R->n_site++;
    }
}

/* ---- Clause budget ----
 * Counted rather than estimated. A pool that runs dry halfway through
 * leaves a formula missing the constraints that would have made it
 * unsatisfiable, which reads as a fault that isn't there. */

static uint32_t
fi_size(const rt_mod_t *M, const cd_lib_t *cd, const fi_res_t *R)
{
    uint32_t i, n = 0;

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type == RT_CELL_COUNT || fi_isff(c->type)) continue;
        if (c->out == 0 || c->out >= M->n_net) continue;
        if (c->type == RT_LUT && cd && c->cdix < cd->n_cell)
            n += cd->cells[c->cdix].n_row;
        else if (c->n_in <= RT_MAX_PIN)
            n += 1u << c->n_in;
    }
    /* Two copies of the logic, four clauses per XOR splice, the ladder at
     * three a site, and the ties and output comparisons over the nets. */
    return 2u * n + 8u * R->n_site + 8u * M->n_net + 256u;
}

/* ---- Encode the combinational half of a module ----
 * Flops contribute nothing, because the cut leaves their outputs free.
 * Where fsel names a node, the driving cell is pointed at a spare variable
 * and XORed into place instead, and that XOR is the fault. */

static int
fi_enc(cn_t *C, const rt_mod_t *M, const cd_lib_t *cd,
       const uint32_t *v, const uint32_t *fsel, rt_ctype_t *bad)
{
    uint32_t i;

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        uint32_t in[RT_MAX_PIN], mask, tgt;
        uint8_t j;

        if (c->type == RT_CELL_COUNT || fi_isff(c->type)) continue;
        if (c->out == 0 || c->out >= M->n_net) continue;

        tgt = (fsel && fsel[c->out]) ? cn_var(C) : v[c->out];
        if (tgt == 0) return -1;

        for (j = 0; j < c->n_in; j++)
            in[j] = (c->ins[j] < M->n_net) ? v[c->ins[j]] : 0;

        if (c->type == RT_CONST) {
            if (cn_unit(C, c->param ? (int32_t)tgt : -(int32_t)tgt) != 0)
                return -1;
        } else if (c->type == RT_LUT) {
            if (!cd || c->cdix >= cd->n_cell) { *bad = c->type; return -1; }
            if (cn_lut(C, &cd->cells[c->cdix], (uint8_t)c->param,
                       in, c->n_in, tgt) != 0) return -1;
        } else {
            /* Anything the bit-blaster left whole lands here and gets
             * refused by name, because a partial encoding would prove
             * something about a circuit nobody built. */
            if (cn_gmsk(c->type, c->n_in, &mask) != 0) {
                *bad = c->type;
                return -1;
            }
            if (cn_gate(C, mask, in, c->n_in, tgt) != 0) return -1;
        }

        if (tgt != v[c->out] &&
            fi_xor(C, tgt, fsel[c->out], v[c->out]) != 0) return -1;
    }
    return 0;
}

/* ---- At most one selector true ----
 * Sinz's sequential counter, where s means "a fault at or before here".
 * Three clauses a site rather than the n squared a pairwise encoding wants,
 * which matters once a design runs to a few thousand flops. */

static int
fi_amo(cn_t *C, const fi_res_t *R)
{
    uint32_t i, prev, s;
    int32_t cl[2];

    if (R->n_site < 2) return 0;

    prev = cn_var(C);
    cl[0] = -(int32_t)R->site[0].sel; cl[1] = (int32_t)prev;
    if (cn_add(C, cl, 2) != 0) return -1;

    for (i = 1; i < R->n_site; i++) {
        cl[0] = -(int32_t)R->site[i].sel; cl[1] = -(int32_t)prev;
        if (cn_add(C, cl, 2) != 0) return -1;
        if (i + 1 == R->n_site) break;

        s = cn_var(C);
        cl[0] = -(int32_t)R->site[i].sel; cl[1] = (int32_t)s;
        if (cn_add(C, cl, 2) != 0) return -1;
        cl[0] = -(int32_t)prev; cl[1] = (int32_t)s;
        if (cn_add(C, cl, 2) != 0) return -1;
        prev = s;
    }
    return 0;
}

/* ---- Public: prove or disprove single-fault masking ---- */

int
fi_chk(const rt_mod_t *M0, const cd_lib_t *cd, int comb, fi_res_t *R)
{
    cn_t C;
    rt_mod_t *M = NULL;
    uint32_t *va = NULL, *vb = NULL, *fsel = NULL;
    uint8_t *freen = NULL, *obs = NULL, *model = NULL;
    int32_t *cl = NULL;
    rt_ctype_t bad = RT_CELL_COUNT;
    uint32_t i, ncls, nlit;
    int rc = -1;

    if (!M0 || !R) return -1;
    memset(R, 0, sizeof(*R));

    M = fi_clone(M0);
    if (!M) return -1;
    mp_bblst(M);

    fi_site(M, comb, R);
    if (R->n_site == 0) { rt_free(M); free(M); return 0; }

    ncls = fi_size(M, cd, R);
    nlit = 9u * ncls + R->n_site + M->n_net + 64u;
    if (cn_init(&C, ncls, nlit) != 0) { rt_free(M); free(M); return -1; }

    va    = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    vb    = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    fsel  = (uint32_t *)calloc(M->n_net + 1, sizeof(uint32_t));
    freen = (uint8_t *)calloc(M->n_net + 1, 1);
    obs   = (uint8_t *)calloc(M->n_net + 1, 1);
    cl    = (int32_t *)calloc((size_t)M->n_net + R->n_site + 4,
                              sizeof(int32_t));
    if (!va || !vb || !fsel || !freen || !obs || !cl) goto done;

    /* A net driven by a gate takes its value from clauses. Everything else,
     * a port, a clock, a flop output, is the solver's to choose. */
    for (i = 1; i < M->n_net; i++) freen[i] = 1;
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type == RT_CELL_COUNT || fi_isff(c->type)) continue;
        if (c->out > 0 && c->out < M->n_net) freen[c->out] = 0;
    }

    /* Watch the real outputs and every flop D, since an upset that only
     * reaches a D has still landed in the state and will be read back. */
    for (i = 1; i < M->n_net; i++)
        if (M->nets[i].is_port == 2) obs[i] = 1;
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (!fi_isff(c->type)) continue;
        if (c->n_in > 0 && c->ins[0] < M->n_net) obs[c->ins[0]] = 1;
    }

    for (i = 1; i < M->n_net; i++) { va[i] = cn_var(&C); vb[i] = cn_var(&C); }

    /* One driver per net is the normal case, so a repeat means a malformed
     * netlist. Keep the first and drop the rest rather than hand the solver
     * a selector that switches nothing on. */
    {
        uint32_t k = 0;
        for (i = 0; i < R->n_site; i++) {
            if (fsel[R->site[i].net]) continue;
            R->site[k] = R->site[i];
            R->site[k].sel = cn_var(&C);
            fsel[R->site[k].net] = R->site[k].sel;
            k++;
        }
        R->n_site = k;
    }

    if (fi_enc(&C, M, cd, va, NULL, &bad) != 0) goto done;
    if (fi_enc(&C, M, cd, vb, fsel, &bad) != 0) goto done;

    /* Replica agreement, and the whole reason a voted design can be proved
     * at all. Cutting at the flops hands the solver an arbitrary state, so
     * left alone it will start the three copies of a TMR'd flop already
     * disagreeing, walk the vote straight past the voter and call perfectly
     * good hardening broken.
     *
     * Flops that share a D net, a type and a reset hold the same value in
     * every state reachable from reset, by induction on cycles with reset
     * as the base case. That is an invariant of the design rather than a
     * favour to the checker, so assuming it is sound. Grouping by shared D
     * rather than by cone shape keeps it sound with no proof obligation of
     * its own, and costs nothing here because tm_dff hands every replica
     * the original D net in both TMR modes. */
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        uint32_t j;

        if (!fi_isff(c->type)) continue;
        if (c->out == 0 || c->out >= M->n_net) continue;

        for (j = 1; j < i; j++) {
            const rt_cell_t *p = &M->cells[j];
            if (!fi_isff(p->type) || p->type != c->type) continue;
            if (p->out == 0 || p->out >= M->n_net) continue;
            if (p->n_in != c->n_in) continue;
            if (memcmp(p->ins, c->ins, c->n_in * sizeof(uint32_t)) != 0)
                continue;
            if (fi_tie(&C, va[c->out], va[p->out]) != 0) goto done;
            R->n_rep++;
            break;   /* chaining to one peer ties the whole group */
        }
    }

    /* Free nets agree between the copies, unless the fault sits on one */
    for (i = 1; i < M->n_net; i++) {
        if (!freen[i]) continue;
        if (fsel[i]) {
            if (fi_xor(&C, va[i], fsel[i], vb[i]) != 0) goto done;
        } else {
            if (fi_tie(&C, va[i], vb[i]) != 0) goto done;
        }
    }

    /* One XOR per watched net, then assert at least one of them fired */
    {
        uint32_t k = 0;
        for (i = 1; i < M->n_net; i++) {
            uint32_t d;
            if (!obs[i]) continue;
            d = cn_var(&C);
            if (fi_xor(&C, va[i], vb[i], d) != 0) goto done;
            cl[k++] = (int32_t)d;
        }
        R->n_obs = k;
        if (k == 0) { rc = 0; goto done; }
        if (cn_add(&C, cl, k) != 0) goto done;
    }

    for (i = 0; i < R->n_site; i++) cl[i] = (int32_t)R->site[i].sel;
    if (cn_add(&C, cl, R->n_site) != 0) goto done;
    if (fi_amo(&C, R) != 0) goto done;

    model = (uint8_t *)calloc(C.n_var + 2, 1);
    if (!model) goto done;

    /* Every answer names one site that got through, so block it and ask
     * again. A hardened design comes back unsatisfiable the first time and
     * the whole run costs a single solve. */
    {
        KA_GUARD(g, FI_MAXFLT + 1);
        while (g--) {
            int sr = sa_solve(&C, model, FI_CONF);
            uint32_t hit = FI_MAXFLT;

            if (sr == 0) break;
            if (sr < 0) { R->gaveup = 1; break; }

            for (i = 0; i < R->n_site; i++)
                if (model[R->site[i].sel]) { hit = i; break; }
            if (hit == FI_MAXFLT) break;   /* satisfied without a fault */

            R->bad[R->n_bad++] = hit;
            if (cn_unit(&C, -(int32_t)R->site[hit].sel) != 0) break;
        }
    }
    rc = (int)R->n_bad;

done:
    if (rc < 0 && bad != RT_CELL_COUNT)
        fprintf(stderr, "takahe: fi: cannot encode a %s cell, so nothing "
                "was proved\n", rt_cname(bad));
    free(cl); free(model); free(obs); free(freen);
    free(fsel); free(vb); free(va);
    cn_free(&C);
    rt_free(M); free(M);
    return rc;
}

/* ---- Report ---- */

void
fi_rep(const fi_res_t *R)
{
    uint32_t i, nseq = 0;

    if (!R) return;

    for (i = 0; i < R->n_site; i++) if (R->site[i].seq) nseq++;

    printf("takahe: fi: %u sites (%u sequential), %u watched nets\n",
           R->n_site, nseq, R->n_obs);
    if (R->n_rep)
        printf("takahe: fi: %u replica flops assumed to track their peers\n",
               R->n_rep);

    if (R->trunc)
        printf("takahe: fi: over %u sites, only the first were checked\n",
               (unsigned)FI_MAXFLT);

    if (R->n_bad == 0) {
        if (R->gaveup)
            printf("takahe: fi: solver gave up, nothing proved\n");
        else if (R->n_obs == 0)
            printf("takahe: fi: nothing observable, so nothing to prove\n");
        else
            printf("takahe: fi: no single upset reaches an output\n");
        return;
    }

    printf("takahe: fi: %u of %u sites unprotected\n",
           R->n_bad, R->n_site);
    for (i = 0; i < R->n_bad && i < FI_SHOW; i++) {
        const fi_site_t *s = &R->site[R->bad[i]];
        printf("takahe: fi:   %s (%s)\n", s->name, s->seq ? "flop" : "gate");
    }
    if (R->n_bad > FI_SHOW)
        printf("takahe: fi:   and %u more\n", R->n_bad - FI_SHOW);
    if (R->gaveup)
        printf("takahe: fi: solver gave up, so the list may be short\n");
}
