/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */


/*
 * tk_bblst.c -- bit-blast
 *
 * Decomposes multi-bit cells into 1-bit slices that map straight onto
 * library gates. An 8-bit AND becomes eight 1-bit ANDs, a 4-bit adder
 * becomes a ripple-carry chain of full adders.
 */

#include "takahe.h"
/* ---- Bit-blast helpers ---- */

/* Create a 1-bit net named "base_N" */
static uint32_t
bb_net(rt_mod_t *M, uint32_t base, uint32_t bit, uint8_t port)
{
    char nm[64];
    const char *bn = M->strs + M->nets[base].name_off;
    int nl = snprintf(nm, 64, "%.*s_%u",
                      (int)M->nets[base].name_len, bn, bit);
    if (nl < 0 || nl >= 64) nl = 63;
    return rt_anet(M, nm, (uint16_t)nl, 1, port, M->nets[base].radix);
}

/* Scoped temporary: "tag_cellindex_bit" — globally unique */
static uint32_t
bb_tmp(rt_mod_t *M, const char *tag, uint32_t ci, uint32_t bit)
{
    char nm[64];
    int nl = snprintf(nm, 64, "%s_%u_%u", tag, ci, bit);
    if (nl < 0 || nl >= 64) nl = 63;
    return rt_anet(M, nm, (uint16_t)nl, 1, 0, TK_RADIX_BIN);
}

/* Get or create 1-bit slice nets for a multi-bit net.
 * Stores indices in out[0..width-1]. Returns width. */
static uint32_t
bb_zero(rt_mod_t *M, uint32_t ci)
{
    uint32_t z = bb_tmp(M, "z", ci, 0);
    uint32_t cc = rt_acell(M, RT_CONST, z, NULL, 0, 1);
    if (cc > 0 && cc < M->n_cell) M->cells[cc].param = 0;
    return z;
}

static uint32_t
bb_slc(rt_mod_t *M, uint32_t ni, uint32_t *out, uint32_t maxw,
       uint32_t **smap, uint32_t smsz)
{
    uint32_t w, j;
    if (ni == 0 || ni >= M->n_net) return 0;
    w = M->nets[ni].width;
    if (w == 0) w = 1;
    if (w > maxw) w = maxw;

    /* Already 1-bit? */
    if (w == 1) {
        out[0] = ni;
        return 1;
    }

    /* Check slice map for existing split */
    if (*smap && ni < smsz && (*smap)[ni] != 0) {
        uint32_t base_idx = (*smap)[ni];
        for (j = 0; j < w; j++)
            out[j] = base_idx + j;
        return w;
    }

    /* Create new 1-bit nets */
    {
        uint32_t first = M->n_net;
        for (j = 0; j < w; j++)
            out[j] = bb_net(M, ni, j, M->nets[ni].is_port);
        if (*smap && ni < smsz)
            (*smap)[ni] = first;
    }
    return w;
}

/* ---- Bit-blast one cell ---- */

static int
bb_cell(rt_mod_t *M, uint32_t ci, uint32_t **smap, uint32_t smsz)
{
    rt_cell_t *c = &M->cells[ci];
    uint32_t w = c->width;
    uint32_t obits[64], abits[64], bbits[64];
    uint32_t j, ow, aw, bw;
    uint32_t ins1[1], ins2[2], ins3[3];
    int created = 0;

    if (w <= 1) {
        if (!(c->type == RT_SELECT && c->n_in > 0 &&
              c->ins[0] < M->n_net && M->nets[c->ins[0]].width > 1))
            return 0;
    }
    if (w > 64) w = 64;    /* sanity bound */

    switch ((int)c->type) {
    case RT_AND: case RT_OR: case RT_XOR:
    case RT_NAND: case RT_NOR: case RT_XNOR:
    {
        /* Bitwise: w copies, each 1-bit */
        rt_ctype_t ct = c->type;
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        bw = bb_slc(M, c->ins[1], bbits, w, smap, smsz);
        for (j = 0; j < ow && j < aw && j < bw; j++) {
            ins2[0] = abits[j]; ins2[1] = bbits[j];
            rt_acell(M, ct, obits[j], ins2, 2, 1);
            created++;
        }
        c->type = RT_CELL_COUNT; /* kill original */
        break;
    }

    case RT_NOT:
    {
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        for (j = 0; j < ow && j < aw; j++) {
            ins1[0] = abits[j];
            rt_acell(M, RT_NOT, obits[j], ins1, 1, 1);
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_BUF: case RT_ASSIGN:
    {
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        for (j = 0; j < ow && j < aw; j++) {
            ins1[0] = abits[j];
            rt_acell(M, RT_ASSIGN, obits[j], ins1, 1, 1);
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_MUX:
    {
        /* MUX: sel is 1-bit, data paths are w-bit */
        uint32_t sel = c->ins[0];
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[1], abits, w, smap, smsz); /* d0 */
        bw = bb_slc(M, c->ins[2], bbits, w, smap, smsz); /* d1 */
        for (j = 0; j < ow && j < aw && j < bw; j++) {
            ins3[0] = sel; ins3[1] = abits[j]; ins3[2] = bbits[j];
            rt_acell(M, RT_MUX, obits[j], ins3, 3, 1);
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_CONST:
    {
        int64_t val = c->param;
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        for (j = 0; j < ow; j++) {
            uint32_t ci2 = rt_acell(M, RT_CONST, obits[j], NULL, 0, 1);
            if (ci2 > 0 && ci2 < M->n_cell)
                M->cells[ci2].param = (val >> j) & 1;
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_DFF:
    {
        /* w parallel DFFs sharing CLK */
        uint32_t clk = c->ins[1];
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz); /* D */
        for (j = 0; j < ow && j < aw; j++) {
            ins2[0] = abits[j]; ins2[1] = clk;
            rt_acell(M, RT_DFF, obits[j], ins2, 2, 1);
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_DFFR:
    {
        uint32_t clk = c->ins[1];
        uint32_t rst = c->ins[2];
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        for (j = 0; j < ow && j < aw; j++) {
            ins3[0] = abits[j]; ins3[1] = clk; ins3[2] = rst;
            rt_acell(M, RT_DFFR, obits[j], ins3, 3, 1);
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_ADD:
    {
        /* Ripple-carry adder: bit 0 = half adder, bits 1+ = full adder.
         * sum[i]   = a[i] ^ b[i] ^ cin
         * cout[i]  = (a[i] & b[i]) | (cin & (a[i] ^ b[i])) */
        uint32_t carry = 0, zbit = 0;
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        bw = bb_slc(M, c->ins[1], bbits, w, smap, smsz);

        for (j = 0; j < ow; j++) {
            uint32_t axb = bb_tmp(M, "xb", ci, j);
            uint32_t aj, bj;
            if (j >= aw || j >= bw) {
                if (zbit == 0) zbit = bb_zero(M, ci);
            }
            aj = j < aw ? abits[j] : zbit;
            bj = j < bw ? bbits[j] : zbit;
            abits[j] = aj; bbits[j] = bj;
            ins2[0] = aj; ins2[1] = bj;
            rt_acell(M, RT_XOR, axb, ins2, 2, 1);

            if (j == 0) {
                ins1[0] = axb;
                rt_acell(M, RT_ASSIGN, obits[j], ins1, 1, 1);
                carry = bb_tmp(M, "cy", ci, j);
                ins2[0] = abits[j]; ins2[1] = bbits[j];
                rt_acell(M, RT_AND, carry, ins2, 2, 1);
            } else {
                ins2[0] = axb; ins2[1] = carry;
                rt_acell(M, RT_XOR, obits[j], ins2, 2, 1);

                uint32_t ab = bb_tmp(M, "ab", ci, j);
                ins2[0] = abits[j]; ins2[1] = bbits[j];
                rt_acell(M, RT_AND, ab, ins2, 2, 1);

                uint32_t cxb = bb_tmp(M, "cx", ci, j);
                ins2[0] = carry; ins2[1] = axb;
                rt_acell(M, RT_AND, cxb, ins2, 2, 1);

                carry = bb_tmp(M, "cy", ci, j);
                ins2[0] = ab; ins2[1] = cxb;
                rt_acell(M, RT_OR, carry, ins2, 2, 1);
            }
            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_SUB:
    {
        /* a - b = a + ~b + 1.
         * Invert b, then ripple-carry with cin=1. */
        uint32_t carry;
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        bw = bb_slc(M, c->ins[1], bbits, w, smap, smsz);

        carry = bb_tmp(M, "c1", ci, 0);
        {
            uint32_t ci2 = rt_acell(M, RT_CONST, carry, NULL, 0, 1);
            if (ci2 > 0 && ci2 < M->n_cell)
                M->cells[ci2].param = 1;
        }

        for (j = 0; j < ow && j < aw && j < bw; j++) {
            uint32_t nb = bb_tmp(M, "nb", ci, j);
            ins1[0] = bbits[j];
            rt_acell(M, RT_NOT, nb, ins1, 1, 1);

            uint32_t axb = bb_tmp(M, "xb", ci, j);
            ins2[0] = abits[j]; ins2[1] = nb;
            rt_acell(M, RT_XOR, axb, ins2, 2, 1);

            ins2[0] = axb; ins2[1] = carry;
            rt_acell(M, RT_XOR, obits[j], ins2, 2, 1);

            uint32_t ab = bb_tmp(M, "ab", ci, j);
            ins2[0] = abits[j]; ins2[1] = nb;
            rt_acell(M, RT_AND, ab, ins2, 2, 1);

            uint32_t cxb = bb_tmp(M, "cx", ci, j);
            ins2[0] = carry; ins2[1] = axb;
            rt_acell(M, RT_AND, cxb, ins2, 2, 1);

            carry = bb_tmp(M, "cy", ci, j);
            ins2[0] = ab; ins2[1] = cxb;
            rt_acell(M, RT_OR, carry, ins2, 2, 1);

            created++;
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_EQ:
    {
        /* XOR each bit pair, OR-reduce, then invert.
         * eq = ~(|{a[i]^b[i]}) */
        uint32_t acc = 0;
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        bw = bb_slc(M, c->ins[1], bbits, w, smap, smsz);
        /* Note: output is 1-bit, no need to slice */
        for (j = 0; j < aw && j < bw; j++) {
            uint32_t xb = bb_tmp(M, "xb", ci, j);
            ins2[0] = abits[j]; ins2[1] = bbits[j];
            rt_acell(M, RT_XOR, xb, ins2, 2, 1);
            if (j == 0) {
                acc = xb;
            } else {
                uint32_t ored = bb_tmp(M, "or", ci, j);
                ins2[0] = acc; ins2[1] = xb;
                rt_acell(M, RT_OR, ored, ins2, 2, 1);
                acc = ored;
            }
        }
        /* Invert: eq = ~any_diff */
        ins1[0] = acc;
        rt_acell(M, RT_NOT, c->out, ins1, 1, 1);
        c->type = RT_CELL_COUNT;
        created++;
        break;
    }

    case RT_NE:
    {
        /* XOR each bit, OR-reduce */
        uint32_t acc = 0;
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        bw = bb_slc(M, c->ins[1], bbits, w, smap, smsz);
        for (j = 0; j < aw && j < bw; j++) {
            uint32_t xb = bb_tmp(M, "xb", ci, j);
            ins2[0] = abits[j]; ins2[1] = bbits[j];
            rt_acell(M, RT_XOR, xb, ins2, 2, 1);
            if (j == 0) { acc = xb; }
            else {
                uint32_t ored = bb_tmp(M, "or", ci, j);
                ins2[0] = acc; ins2[1] = xb;
                rt_acell(M, RT_OR, ored, ins2, 2, 1);
                acc = ored;
            }
        }
        ins1[0] = acc;
        rt_acell(M, RT_ASSIGN, c->out, ins1, 1, 1);
        c->type = RT_CELL_COUNT;
        created++;
        break;
    }

    case RT_LT: case RT_LE: case RT_GT: case RT_GE:
    {
        /* Ripple comparator: MSB-first subtraction, check sign bit.
         * For simplicity, use SUB bit-blast pattern and check carry. */
        /* Defer: emit as-is (1-bit output, operands already compared).
         * If operands are multi-bit, the width field is the operand width,
         * but output is always 1-bit — no blast needed for output.
         * Inputs still need blasting if they feed here. */
        break; /* leave as-is for now */
    }

    case RT_CONCAT:
    {
        /* Just wire renaming: output bits connect to input bits */
        uint32_t bit = 0;
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        for (j = 0; j < c->n_in && j < RT_MAX_PIN; j++) {
            uint32_t iw = c->ins[j] < M->n_net ?
                M->nets[c->ins[j]].width : 1;
            uint32_t ibits[64];
            uint32_t k, iaw;
            if (iw > 64) iw = 64;
            iaw = bb_slc(M, c->ins[j], ibits, iw, smap, smsz);
            for (k = 0; k < iaw && bit < ow; k++, bit++) {
                ins1[0] = ibits[k];
                rt_acell(M, RT_ASSIGN, obits[bit], ins1, 1, 1);
                created++;
            }
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_SELECT:
    {
        /* Wire renaming: output bit i = input bit (offset + i) */
        uint32_t lo = (uint32_t)(c->param & 0xFFFF);
        uint32_t iw = M->nets[c->ins[0]].width;
        uint32_t ibits[64];
        uint32_t iaw;
        if (iw > 64) iw = 64;
        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        iaw = bb_slc(M, c->ins[0], ibits, iw, smap, smsz);
        for (j = 0; j < ow; j++) {
            uint32_t src = lo + j;
            if (src < iaw) {
                ins1[0] = ibits[src];
                rt_acell(M, RT_ASSIGN, obits[j], ins1, 1, 1);
                created++;
            }
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_MUL:
    {
        uint32_t acc[64];
        uint32_t zero;
        uint32_t i, k;

        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        bw = bb_slc(M, c->ins[1], bbits, w, smap, smsz);
        if (ow == 0 || aw == 0 || bw == 0 || ow > 64u) break;

        zero = bb_zero(M, ci);

        for (k = 0; k < ow; k++) {
            if (k < aw) {
                acc[k] = bb_tmp(M, "pp", ci, k);
                ins2[0] = abits[k]; ins2[1] = bbits[0];
                rt_acell(M, RT_AND, acc[k], ins2, 2, 1);
                created++;
            } else {
                acc[k] = zero;
            }
        }

        for (i = 1; i < bw; i++) {
            uint32_t carry = 0;
            for (k = i; k < ow; k++) {
                uint32_t rb, axb, nx;
                if (k - i < aw) {
                    rb = bb_tmp(M, "pp", ci, (uint32_t)(i * ow + k));
                    ins2[0] = abits[k - i]; ins2[1] = bbits[i];
                    rt_acell(M, RT_AND, rb, ins2, 2, 1);
                    created++;
                } else if (carry == 0) {
                    break;
                } else {
                    rb = zero;
                }

                axb = bb_tmp(M, "mx", ci, (uint32_t)(i * ow + k));
                ins2[0] = acc[k]; ins2[1] = rb;
                rt_acell(M, RT_XOR, axb, ins2, 2, 1);

                nx = bb_tmp(M, "ms", ci, (uint32_t)(i * ow + k));
                if (carry == 0) {
                    ins1[0] = axb;
                    rt_acell(M, RT_ASSIGN, nx, ins1, 1, 1);
                    carry = bb_tmp(M, "mc", ci, (uint32_t)(i * ow + k));
                    ins2[0] = acc[k]; ins2[1] = rb;
                    rt_acell(M, RT_AND, carry, ins2, 2, 1);
                } else {
                    uint32_t ab, cx, nc;
                    ins2[0] = axb; ins2[1] = carry;
                    rt_acell(M, RT_XOR, nx, ins2, 2, 1);

                    ab = bb_tmp(M, "ma", ci, (uint32_t)(i * ow + k));
                    ins2[0] = acc[k]; ins2[1] = rb;
                    rt_acell(M, RT_AND, ab, ins2, 2, 1);

                    cx = bb_tmp(M, "mk", ci, (uint32_t)(i * ow + k));
                    ins2[0] = carry; ins2[1] = axb;
                    rt_acell(M, RT_AND, cx, ins2, 2, 1);

                    nc = bb_tmp(M, "mc", ci, (uint32_t)(i * ow + k));
                    ins2[0] = ab; ins2[1] = cx;
                    rt_acell(M, RT_OR, nc, ins2, 2, 1);
                    carry = nc;
                }
                acc[k] = nx;
                created++;
            }
        }

        for (k = 0; k < ow; k++) {
            ins1[0] = acc[k];
            rt_acell(M, RT_ASSIGN, obits[k], ins1, 1, 1);
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_SHL: case RT_SHR: case RT_SHRA:
    {
        uint32_t cur[64], nxt[64], sbits[64];
        uint32_t sw, zbit = 0, fill, st, k;
        int left = (c->type == RT_SHL);
        int arith = (c->type == RT_SHRA);
        int64_t amt = 0;
        uint32_t sdrv;

        ow = bb_slc(M, c->out, obits, w, smap, smsz);
        aw = bb_slc(M, c->ins[0], abits, w, smap, smsz);
        if (ow == 0 || aw == 0 || ow > 64) break;

        for (k = 0; k < ow; k++)
            cur[k] = k < aw ? abits[k] : (zbit ? zbit : (zbit = bb_zero(M, ci)));

        fill = arith ? cur[aw - 1] : (zbit ? zbit : (zbit = bb_zero(M, ci)));

        sdrv = c->ins[1] < M->n_net ? M->nets[c->ins[1]].driver : 0;
        if (sdrv != 0 && sdrv < M->n_cell &&
            M->cells[sdrv].type == RT_CONST) {
            amt = M->cells[sdrv].param;
            if (amt < 0) amt = 0;
            for (k = 0; k < ow; k++) {
                uint32_t src;
                int have;
                if (left) {
                    have = ((int64_t)k >= amt);
                    src = have ? cur[(uint32_t)((int64_t)k - amt)] : fill;
                } else {
                    have = ((int64_t)k + amt < (int64_t)ow);
                    src = have ? cur[(uint32_t)((int64_t)k + amt)] : fill;
                }
                ins1[0] = src;
                rt_acell(M, RT_ASSIGN, obits[k], ins1, 1, 1);
                created++;
            }
            c->type = RT_CELL_COUNT;
            break;
        }

        sw = bb_slc(M, c->ins[1], sbits, w, smap, smsz);
        if (sw == 0) break;

        for (st = 0; st < sw && (1u << st) < ow; st++) {
            uint32_t d = 1u << st;
            for (k = 0; k < ow; k++) {
                uint32_t alt;
                if (left)
                    alt = (k >= d) ? cur[k - d] : fill;
                else
                    alt = (k + d < ow) ? cur[k + d] : fill;
                nxt[k] = bb_tmp(M, "sb", ci, (uint32_t)(st * ow + k));
                ins3[0] = sbits[st]; ins3[1] = cur[k]; ins3[2] = alt;
                rt_acell(M, RT_MUX, nxt[k], ins3, 3, 1);
                created++;
            }
            for (k = 0; k < ow; k++) cur[k] = nxt[k];
        }

        for (k = 0; k < ow; k++) {
            ins1[0] = cur[k];
            rt_acell(M, RT_ASSIGN, obits[k], ins1, 1, 1);
        }
        c->type = RT_CELL_COUNT;
        break;
    }

    case RT_PMUX:
    case RT_DLAT:
    case RT_MEMRD: case RT_MEMWR:
        /* Not bit-blasted in Tier 5 — left as black-box */
        break;

    case RT_CELL_COUNT:
    default:
        break;
    }

    return created;
}

/* ---- Public: bit-blast all multi-bit cells ---- */

int
mp_bblst(rt_mod_t *M)
{
    uint32_t i, orig;
    int total = 0;
    uint32_t *smap;  /* net → first 1-bit slice index */

    if (!M) return 0;

    if (M->n_net > TK_MAX_NETS - 1024) return -1;
    smap = (uint32_t *)calloc(M->n_net + 1024, sizeof(uint32_t));
    if (!smap) return -1;

    /* Process original cells only (not newly created ones) */
    orig = M->n_cell;
    for (i = 1; i < orig; i++) {
        if (M->cells[i].type == RT_CELL_COUNT) continue;
        total += bb_cell(M, i, &smap, M->n_net + 1024);
    }

    free(smap);
    printf("takahe: bit-blast: %d cells created (%u total)\n",
           total, M->n_cell - 1);
    return total;
}
