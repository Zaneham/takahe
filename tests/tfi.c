/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tfi.c -- fault injection
 * A hardening checker that says everything is fine is indistinguishable
 * from no hardening checker at all, so both answers get tested. */

#include "tharns.h"
#include "takahe.h"

/* ---- One flop, output straight out, nothing protecting it ---- */

static rt_mod_t *
fi_bare(void)
{
    rt_mod_t *M = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    uint32_t d, q, ins[1];

    if (!M) return NULL;
    if (rt_init(M, 64, 64) != 0) { free(M); return NULL; }

    d = rt_anet(M, "d", 1, 1, 1, TK_RADIX_BIN);
    q = rt_anet(M, "q", 1, 1, 2, TK_RADIX_BIN);
    ins[0] = d;
    rt_acell(M, RT_DFF, q, ins, 1, 1);
    return M;
}

static void
fi_open(void)
{
    rt_mod_t *M = fi_bare();
    fi_res_t *R = (fi_res_t *)malloc(sizeof(fi_res_t));

    CHECK(M != NULL && R != NULL);
    CHECK(fi_chk(M, NULL, 0, R) == 1);
    CHECK(R->n_site == 1);
    CHECK(R->n_bad == 1);
    CHECK(R->site[R->bad[0]].seq == 1);

    free(R); rt_free(M); free(M);
    PASS();
}
TH_REG("fi", fi_open)

/* ---- The same flop after TMR, which should now survive any one upset ---- */

static void
fi_tmrok(void)
{
    rt_mod_t *M = fi_bare();
    fi_res_t *R = (fi_res_t *)malloc(sizeof(fi_res_t));

    CHECK(M != NULL && R != NULL);
    CHECK(tm_tmr(M, 0) == 1);
    CHECK(fi_chk(M, NULL, 0, R) == 0);
    CHECK(R->n_site == 3);          /* three replica flops */
    CHECK(R->n_bad == 0);

    free(R); rt_free(M); free(M);
    PASS();
}
TH_REG("fi", fi_tmrok)

/* ---- Three flops behind a voter that only reads two of them ----
 * This is what an optimiser leaves behind when it decides two of the three
 * cones were the same cone. The area is still paid for and the voter still
 * looks like a voter, so nothing downstream notices. */

static void
fi_merge(void)
{
    rt_mod_t *M = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    fi_res_t *R = (fi_res_t *)malloc(sizeof(fi_res_t));
    uint32_t d, qa, qb, t0, t1, t2, t3, q, ins[2];

    CHECK(M != NULL && R != NULL);
    CHECK(rt_init(M, 64, 64) == 0);

    d  = rt_anet(M, "d",  1, 1, 1, TK_RADIX_BIN);
    qa = rt_anet(M, "qa", 2, 1, 0, TK_RADIX_BIN);
    qb = rt_anet(M, "qb", 2, 1, 0, TK_RADIX_BIN);
    t0 = rt_anet(M, "t0", 2, 1, 0, TK_RADIX_BIN);
    t1 = rt_anet(M, "t1", 2, 1, 0, TK_RADIX_BIN);
    t2 = rt_anet(M, "t2", 2, 1, 0, TK_RADIX_BIN);
    t3 = rt_anet(M, "t3", 2, 1, 0, TK_RADIX_BIN);
    q  = rt_anet(M, "q",  1, 1, 2, TK_RADIX_BIN);

    ins[0] = d;  rt_acell(M, RT_DFF, qa, ins, 1, 1);
    ins[0] = d;  rt_acell(M, RT_DFF, qb, ins, 1, 1);

    /* A majority vote over qa, qb, qb, which is just qb wearing a hat */
    ins[0] = qa; ins[1] = qb; rt_acell(M, RT_AND, t0, ins, 2, 1);
    ins[0] = qb; ins[1] = qb; rt_acell(M, RT_AND, t1, ins, 2, 1);
    ins[0] = qa; ins[1] = qb; rt_acell(M, RT_AND, t2, ins, 2, 1);
    ins[0] = t0; ins[1] = t1; rt_acell(M, RT_OR,  t3, ins, 2, 1);
    ins[0] = t3; ins[1] = t2; rt_acell(M, RT_OR,  q,  ins, 2, 1);

    CHECK(fi_chk(M, NULL, 0, R) == 1);
    CHECK(R->n_site == 2);
    CHECK(R->n_bad == 1);
    CHECK(R->site[R->bad[0]].net == qb);   /* qa still votes, qb decides */

    free(R); rt_free(M); free(M);
    PASS();
}
TH_REG("fi", fi_merge)

/* ---- Purely combinational, so nothing to upset in sequential mode ---- */

static void
fi_nostate(void)
{
    rt_mod_t *M = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    fi_res_t *R = (fi_res_t *)malloc(sizeof(fi_res_t));
    uint32_t a, b, y, ins[2];

    CHECK(M != NULL && R != NULL);
    CHECK(rt_init(M, 64, 64) == 0);

    a = rt_anet(M, "a", 1, 1, 1, TK_RADIX_BIN);
    b = rt_anet(M, "b", 1, 1, 1, TK_RADIX_BIN);
    y = rt_anet(M, "y", 1, 1, 2, TK_RADIX_BIN);
    ins[0] = a; ins[1] = b;
    rt_acell(M, RT_AND, y, ins, 2, 1);

    CHECK(fi_chk(M, NULL, 0, R) == 0);
    CHECK(R->n_site == 0);

    /* In gate mode the one gate it has drives the output directly, so
     * there is nothing between the fault and the pin. */
    CHECK(fi_chk(M, NULL, 1, R) == 1);
    CHECK(R->n_site == 1);
    CHECK(R->n_bad == 1);

    free(R); rt_free(M); free(M);
    PASS();
}
TH_REG("fi", fi_nostate)

/* ---- A gate whose output cannot be seen ----
 * y is ANDed with a hard zero, so nothing that happens upstream of it can
 * reach the pin. The tie cell and the final AND are both still exposed. */

static void
fi_mask(void)
{
    rt_mod_t *M = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    fi_res_t *R = (fi_res_t *)malloc(sizeof(fi_res_t));
    uint32_t a, b, t, z, y, ins[2];
    uint32_t i, seen = 0;

    CHECK(M != NULL && R != NULL);
    CHECK(rt_init(M, 64, 64) == 0);

    a = rt_anet(M, "a", 1, 1, 1, TK_RADIX_BIN);
    b = rt_anet(M, "b", 1, 1, 1, TK_RADIX_BIN);
    t = rt_anet(M, "t", 1, 1, 0, TK_RADIX_BIN);
    z = rt_anet(M, "z", 1, 1, 0, TK_RADIX_BIN);
    y = rt_anet(M, "y", 1, 1, 2, TK_RADIX_BIN);

    ins[0] = a; ins[1] = b; rt_acell(M, RT_XOR, t, ins, 2, 1);
    rt_acell(M, RT_CONST, z, NULL, 0, 1);        /* param defaults to 0 */
    ins[0] = t; ins[1] = z; rt_acell(M, RT_AND, y, ins, 2, 1);

    CHECK(fi_chk(M, NULL, 1, R) >= 0);
    CHECK(R->n_site == 3);

    for (i = 0; i < R->n_bad; i++)
        if (R->site[R->bad[i]].net == t) seen = 1;
    CHECK(seen == 0);               /* t is dead behind the zero */
    CHECK(R->n_bad == 2);           /* the tie and the AND are not */

    free(R); rt_free(M); free(M);
    PASS();
}
TH_REG("fi", fi_mask)
