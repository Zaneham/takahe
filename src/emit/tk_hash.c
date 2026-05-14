/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_hash.c -- Synthesis fingerprint for Takahe RTL
 *
 * Hash a netlist deterministically. Two runs on the same
 * input produce the same digest. When they don't, something
 * has crept in that you didn't intend: a drifted pass
 * iteration order, an uninitialised byte, a clock cycle
 * the gremlins won at darts.
 *
 * FNV-1a, 64 bits. Not a cryptographic hash. It won't
 * survive an adversary but it will catch the day your
 * pipeline turned non-deterministic, which is the one
 * thing anyone actually wants to know about a synthesis
 * tool. IBM used CRCs to detect tape errors for forty
 * years before anyone needed SHA-anything. The principle
 * holds.
 *
 * Canonical form: bump HS_VER any time you change what
 * goes into the hash or in what order. Saved digests
 * become meaningfully invalid the moment the form changes,
 * like postage stamps after a regime change.
 */

#include "takahe.h"

#define HS_VER     1u
#define HS_OFFSET  0xcbf29ce484222325ULL
#define HS_PRIME   0x100000001b3ULL

/* ---- FNV-1a primitives ----
 * Twelve lines, no dependencies, faster than fetching
 * SHA-256 off the internet. A measurable engineering
 * virtue. */

static uint64_t
hs_byte(uint64_t h, uint8_t b)
{
    h ^= (uint64_t)b;
    h *= HS_PRIME;
    return h;
}

static uint64_t
hs_u32(uint64_t h, uint32_t v)
{
    h = hs_byte(h, (uint8_t)( v        & 0xffu));
    h = hs_byte(h, (uint8_t)((v >>  8) & 0xffu));
    h = hs_byte(h, (uint8_t)((v >> 16) & 0xffu));
    h = hs_byte(h, (uint8_t)((v >> 24) & 0xffu));
    return h;
}

static uint64_t
hs_u64(uint64_t h, uint64_t v)
{
    h = hs_u32(h, (uint32_t)( v        & 0xffffffffULL));
    h = hs_u32(h, (uint32_t)( v >> 32));
    return h;
}

/* ---- Public: hash a module ----
 * Walks nets then cells in array order. The walk order
 * is part of the canonical form: anyone changing pool
 * allocation strategy must bump HS_VER. Cell and net
 * indices are deterministic per run on the same input,
 * which is all a fingerprint needs to be useful.
 *
 * Names are NOT hashed. A wire renamed from q to q_r is
 * the same hardware. The hash agrees. */

uint64_t
mp_hash(const rt_mod_t *M)
{
    uint64_t h = HS_OFFSET;
    uint32_t i;
    uint8_t  j;

    if (!M) return 0;

    /* Magic + version. Later versions diverge from byte one,
     * so a v1 digest will never collide with a v2 digest by
     * accident. Belt, braces, and a length of fencing wire. */
    h = hs_byte(h, (uint8_t)'T');
    h = hs_byte(h, (uint8_t)'K');
    h = hs_byte(h, (uint8_t)'H');
    h = hs_byte(h, (uint8_t)'1');
    h = hs_u32(h, HS_VER);

    /* Nets. Index 0 is the sentinel; every pool reserves it
     * for "nobody's home". Skip it deliberately. */
    h = hs_u32(h, M->n_net);
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        h = hs_u32 (h, n->width);
        h = hs_byte(h, n->radix);
        h = hs_byte(h, n->is_port);
        h = hs_u32 (h, n->driver);
    }

    /* Cells. RT_CELL_COUNT marks a slot vacated by some
     * pass — record the gap as a single zero byte so a
     * deletion doesn't collide with an unrelated rewrite. */
    h = hs_u32(h, M->n_cell);
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type == RT_CELL_COUNT) {
            h = hs_byte(h, 0);
            continue;
        }
        h = hs_byte(h, (uint8_t)c->type);
        h = hs_u32 (h, c->width);
        h = hs_u32 (h, c->out);
        h = hs_byte(h, c->n_in);
        for (j = 0; j < c->n_in; j++)
            h = hs_u32(h, c->ins[j]);
        h = hs_u64(h, (uint64_t)c->param);
    }

    return h;
}
