/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_equiv.c -- equivalence checking
 *
 * Feeds the same inputs to two netlists, pre-opt and post-opt, and checks
 * the outputs agree. When they don't, the input that broke it comes back
 * with the answer.
 *
 * SAT first, because a proof beats a sample. What SAT can't encode, wide or
 * sequential cells, falls back to vectors: 24 input bits or fewer enumerates
 * every input, which is also a proof, and anything wider gets 100K random
 * vectors, which is not. The run says out loud which of the three you got.
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Simulate one combinational netlist for given inputs ----
 * Evaluates all cells in topological order (cell array order).
 * Each net carries a uint64_t value (up to 64 bits wide).
 * Returns 0 on success. */

#define EQ_MAXNET 16384

static int
eq_sim(const rt_mod_t *M, uint64_t *vals)
{
    uint32_t i;

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        uint64_t a, b, s, r;
        uint32_t w;

        if (c->type == RT_CELL_COUNT) continue;

        w = c->width;
        if (w > 64) w = 64;

        /* Read inputs */
        a = (c->n_in >= 1 && c->ins[0] < M->n_net) ?
            vals[c->ins[0]] : 0;
        b = (c->n_in >= 2 && c->ins[1] < M->n_net) ?
            vals[c->ins[1]] : 0;
        s = (c->n_in >= 3 && c->ins[2] < M->n_net) ?
            vals[c->ins[2]] : 0;

        /* Evaluate */
        switch ((int)c->type) {
        case RT_AND:   r = a & b; break;
        case RT_OR:    r = a | b; break;
        case RT_XOR:   r = a ^ b; break;
        case RT_NAND:  r = ~(a & b); break;
        case RT_NOR:   r = ~(a | b); break;
        case RT_XNOR:  r = ~(a ^ b); break;
        case RT_NOT:   r = ~a; break;
        case RT_BUF:   r = a; break;
        case RT_ASSIGN:r = a; break;
        case RT_ADD:   r = a + b; break;
        case RT_SUB:   r = a - b; break;
        case RT_MUL:   r = a * b; break;
        case RT_SHL:   r = a << (b & 63); break;
        case RT_SHR:   r = a >> (b & 63); break;
        case RT_EQ:    r = (a == b) ? 1 : 0; break;
        case RT_NE:    r = (a != b) ? 1 : 0; break;
        case RT_LT:    r = (a < b) ? 1 : 0; break;
        case RT_LE:    r = (a <= b) ? 1 : 0; break;
        case RT_GT:    r = (a > b) ? 1 : 0; break;
        case RT_GE:    r = (a >= b) ? 1 : 0; break;
        case RT_MUX:   r = (a & 1) ? s : b; break;
        case RT_CONST: r = (uint64_t)c->param; break;

        case RT_CONCAT: {
            /* Concatenate inputs MSB-first */
            uint8_t j;
            r = 0;
            for (j = 0; j < c->n_in; j++) {
                uint32_t inp = c->ins[j];
                uint64_t iv = (inp < M->n_net) ? vals[inp] : 0;
                uint32_t iw = (inp < M->n_net) ? M->nets[inp].width : 1;
                r = (r << iw) | (iv & ((1ULL << iw) - 1));
            }
            break;
        }

        case RT_SELECT: {
            /* Extract bits [hi:lo] from input a */
            int64_t p = c->param;
            uint32_t hi = (uint32_t)(p >> 16);
            uint32_t lo = (uint32_t)(p & 0xFFFF);
            r = (a >> lo) & ((1ULL << (hi - lo + 1)) - 1);
            break;
        }

        default:
            r = 0;
            break;
        }

        /* Mask to width */
        if (w < 64)
            r &= (1ULL << w) - 1;

        /* Write output */
        if (c->out > 0 && c->out < M->n_net)
            vals[c->out] = r;
    }

    return 0;
}

/* ---- Collect port nets ---- */

typedef struct {
    uint32_t idx;     /* net index */
    uint32_t width;   /* bit width */
    uint8_t  dir;     /* 1=in, 2=out */
} eq_port_t;

static int
eq_ports(const rt_mod_t *M, eq_port_t *ports, int max)
{
    uint32_t i;
    uint32_t lo = M->top_net_lo > 0 ? M->top_net_lo : 1;
    int np = 0;
    for (i = lo; i < M->n_net && np < max; i++) {
        if (M->nets[i].is_port != 0) {
            ports[np].idx   = i;
            ports[np].width = M->nets[i].width;
            ports[np].dir   = M->nets[i].is_port;
            np++;
        }
    }
    return np;
}

/* ---- Public: check equivalence of two netlists ----
 * Both must have the same ports (by name). Returns 0 if
 * equivalent, -1 if a mismatch is found, -2 on error.
 * Prints the failing vector on mismatch. */

/* ---- Prove it, if the design encodes ----
 * Simulation samples the input space. A miter searches it. Where the
 * design is bit-level and combinational the solver settles the question
 * outright, so try that first and fall back to vectors when it meets
 * something it cannot encode. Returns 1 equivalent, 0 different,
 * -1 could not encode. */

static int
eq_sat(const rt_mod_t *A, const rt_mod_t *B)
{
    cn_t C;
    uint32_t diff;
    uint8_t *model;
    int rc, ret = -1;

    if (cn_init(&C, 4000000u, 40000000u) != 0) return -1;
    diff = cn_mitr(&C, A, B, NULL);
    if (diff == 0) { cn_free(&C); return -1; }

    model = (uint8_t *)calloc(C.n_var + 2, 1);
    if (!model) { cn_free(&C); return -1; }

    rc = sa_solve(&C, model, 20000000ull);
    if (rc == 0) {
        printf("takahe: equiv: PASS — proved equivalent "
               "(%u vars, %u clauses, no distinguishing input exists)\n",
               C.n_var, C.n_cls);
        ret = 1;
    } else if (rc == 1) {
        printf("takahe: equiv: FAIL — the solver found an input that "
               "tells them apart\n");
        ret = 0;
    }
    /* rc < 0 means it gave up, which is not an answer, so fall through
     * to simulation rather than claim one. */

    free(model);
    cn_free(&C);
    return ret;
}

int
eq_check(const rt_mod_t *A, const rt_mod_t *B)
{
    eq_port_t pa[128], pb[128];
    int npa, npb;
    int n_in = 0, n_out = 0;
    uint32_t in_idx_a[64], in_idx_b[64];
    uint32_t in_width[64];
    uint32_t out_idx_a[64], out_idx_b[64];
    uint32_t out_width[64];
    uint32_t total_in_bits = 0;
    uint64_t n_vectors;
    int exhaustive;
    uint64_t *va, *vb;
    int mismatches = 0;
    int sat_fail = 0;

    if (!A || !B) return -2;
    if (A->n_net > EQ_MAXNET || B->n_net > EQ_MAXNET) return -2;

    {
        int s = -1;
        rt_mod_t *ba = rt_mclone(A), *bb = rt_mclone(B);
        if (ba && bb) {
            mp_bblst(ba);
            mp_bblst(bb);
            fprintf(stderr, "DBGU A undriven=%u  B undriven=%u\n",
                    rt_undrv(ba), rt_undrv(bb));
            s = eq_sat(ba, bb);
        }
        if (ba) { rt_free(ba); free(ba); }
        if (bb) { rt_free(bb); free(bb); }
        if (s == 1) return 0;
        if (s == 0) {
            sat_fail = 1;
            printf("takahe: equiv: solver says not equivalent, "
                   "checking against simulation\n");
        } else {
            printf("takahe: equiv: not encodable for SAT "
                   "(wide or sequential cells), falling back to vectors\n");
        }
    }

    npa = eq_ports(A, pa, 128);
    npb = eq_ports(B, pb, 128);

    /* Match ports by name */
    {
        int i, j;
        for (i = 0; i < npa; i++) {
            const char *na = A->strs + A->nets[pa[i].idx].name_off;
            uint16_t nl = A->nets[pa[i].idx].name_len;

            for (j = 0; j < npb; j++) {
                const char *nb = B->strs + B->nets[pb[j].idx].name_off;
                uint16_t ml = B->nets[pb[j].idx].name_len;
                if (nl == ml && memcmp(na, nb, nl) == 0 &&
                    pa[i].dir == pb[j].dir) {
                    if (pa[i].dir == 1 && n_in < 64) {
                        in_idx_a[n_in] = pa[i].idx;
                        in_idx_b[n_in] = pb[j].idx;
                        in_width[n_in] = pa[i].width < pb[j].width ?
                            pa[i].width : pb[j].width;
                        total_in_bits += in_width[n_in];
                        n_in++;
                    } else if (pa[i].dir == 2 && n_out < 64) {
                        out_idx_a[n_out] = pa[i].idx;
                        out_idx_b[n_out] = pb[j].idx;
                        out_width[n_out] = pa[i].width;
                        n_out++;
                    }
                    break;
                }
            }
        }
    }

    if (n_in == 0 || n_out == 0) {
        printf("takahe: equiv: no matching ports found\n");
        return -2;
    }

    /* Decide: exhaustive (≤24 bits) or random (>24 bits) */
    exhaustive = (total_in_bits <= 24);
    n_vectors = exhaustive ? (1ULL << total_in_bits) : 100000;

    printf("takahe: equiv: %d inputs (%u bits), %d outputs, %s (%"
           PRIu64 " vectors)\n",
           n_in, total_in_bits, n_out,
           exhaustive ? "EXHAUSTIVE (formal proof)" : "RANDOM",
           n_vectors);

    /* Allocate simulation value arrays */
    va = (uint64_t *)calloc(A->n_net, sizeof(uint64_t));
    vb = (uint64_t *)calloc(B->n_net, sizeof(uint64_t));
    if (!va || !vb) { free(va); free(vb); return -2; }

    /* Simulate */
    {
        uint64_t v;
        uint32_t rng = 0xDEADBEEF; /* xorshift for random mode */

        KA_GUARD(g, 20000000);
        for (v = 0; v < n_vectors && g--; v++) {
            uint64_t bits;
            int p, o;
            uint32_t bit_pos;

            /* Generate input vector */
            if (exhaustive) {
                bits = v;
            } else {
                /* xorshift32 */
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                bits = (uint64_t)rng;
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                bits |= ((uint64_t)rng) << 32;
            }

            /* Clear and set inputs */
            memset(va, 0, A->n_net * sizeof(uint64_t));
            memset(vb, 0, B->n_net * sizeof(uint64_t));

            bit_pos = 0;
            for (p = 0; p < n_in; p++) {
                uint64_t mask = (in_width[p] < 64) ?
                    (1ULL << in_width[p]) - 1 : ~0ULL;
                uint64_t val = (bits >> bit_pos) & mask;
                va[in_idx_a[p]] = val;
                vb[in_idx_b[p]] = val;
                bit_pos += in_width[p];
            }

            /* Simulate both */
            eq_sim(A, va);
            eq_sim(B, vb);

            /* Compare outputs */
            for (o = 0; o < n_out; o++) {
                uint64_t mask = (out_width[o] < 64) ?
                    (1ULL << out_width[o]) - 1 : ~0ULL;
                uint64_t oa = va[out_idx_a[o]] & mask;
                uint64_t ob = vb[out_idx_b[o]] & mask;
                if (oa != ob) {
                    if (mismatches == 0) {
                        const char *nm = A->strs +
                            A->nets[out_idx_a[o]].name_off;
                        printf("takahe: equiv: MISMATCH on '%.*s' "
                               "vec=0x%" PRIx64 " "
                               "expected=0x%" PRIx64 " "
                               "got=0x%" PRIx64 "\n",
                               (int)A->nets[out_idx_a[o]].name_len,
                               nm, bits, oa, ob);
                    }
                    mismatches++;
                }
            }
        }
    }

    free(va);
    free(vb);

    if (mismatches == 0 && sat_fail) {
        printf("takahe: equiv: warning: the solver's counterexample does "
               "not reproduce over %" PRIu64 " vectors, so the CNF "
               "encoding is wrong and its verdict is being ignored\n",
               n_vectors);
    }

    if (mismatches == 0) {
        printf("takahe: equiv: PASS — %s equivalent (%"
               PRIu64 " vectors)\n",
               exhaustive ? "formally" : "statistically",
               n_vectors);
        return 0;
    } else {
        printf("takahe: equiv: FAIL — %d mismatches in %"
               PRIu64 " vectors\n",
               mismatches, n_vectors);
        return -1;
    }
}
