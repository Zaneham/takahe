/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_rtl.c -- RTL intermediate representation for Takahe
 *
 * The RTL IR is where SystemVerilog stops being text and starts
 * being hardware. Nets carry signals. Cells implement logic.
 * Flip-flops store state. Everything has a width and a driver.
 *
 * This is the representation that the optimiser will transform
 * and the mapper will consume. It's the lingua franca between
 * "what the designer wrote" and "what the foundry builds."
 *
 * Two pool types (TPF-style, fixed-size, pre-allocated):
 *   rt_net_t  — signals (wires, ports, internal)
 *   rt_cell_t — logic instances (DFF, AND, OR, MUX, etc.)
 *
 * Handles are Burroughs-style tagged references with generation
 * counters. Use-after-delete returns error, not corruption.
 *
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Cell type names ---- */

static const char *ctnames[] = {
    "DFF", "DFFR", "DLAT",
    "AND", "OR", "XOR", "NAND", "NOR", "XNOR",
    "NOT", "BUF", "MUX",
    "ADD", "SUB", "MUL",
    "SHL", "SHR", "SHRA",
    "EQ", "NE", "LT", "LE", "GT", "GE",
    "CONST", "ASSIGN", "CONCAT", "SELECT", "PMUX",
    "MEMRD", "MEMWR"
};

const char *
rt_cname(rt_ctype_t t)
{
    if (t < RT_CELL_COUNT) return ctnames[t];
    return "???";
}

/* ---- Init / Free ---- */

int
rt_init(rt_mod_t *M, uint32_t max_net, uint32_t max_cell)
{
    if (!M) return -1;
    memset(M, 0, sizeof(*M));

    M->nets = (rt_net_t *)calloc(max_net, sizeof(rt_net_t));
    M->cells = (rt_cell_t *)calloc(max_cell, sizeof(rt_cell_t));
    M->strs = (char *)calloc(1, 1024 * 1024);  /* 1MB string pool */

    if (!M->nets || !M->cells || !M->strs) {
        free(M->nets); free(M->cells); free(M->strs);
        memset(M, 0, sizeof(*M));
        return -1;
    }

    M->max_net  = max_net;
    M->max_cell = max_cell;
    M->str_max  = 1024 * 1024;
    M->str_len  = 1;  /* 0 = sentinel */
    M->n_net    = 1;  /* 0 = sentinel */
    M->n_cell   = 1;  /* 0 = sentinel */

    return 0;
}

void
rt_free(rt_mod_t *M)
{
    if (!M) return;
    free(M->nets);
    free(M->cells);
    free(M->strs);
    memset(M, 0, sizeof(*M));
}

/* ---- String interning ---- */

static uint32_t
rt_sint(rt_mod_t *M, const char *s, uint16_t len)
{
    uint32_t off;
    if (M->str_len + len + 1 > M->str_max) return 0;
    off = M->str_len;
    memcpy(M->strs + off, s, len);
    M->strs[off + len] = '\0';
    M->str_len += len + 1;
    return off;
}

/* ---- Add net ---- */

uint32_t
rt_anet(rt_mod_t *M, const char *name, uint16_t nlen,
        uint32_t width, uint8_t port, uint8_t radix)
{
    uint32_t idx;
    rt_net_t *n;

    if (M->n_net >= M->max_net) return 0;

    idx = M->n_net++;
    n = &M->nets[idx];
    memset(n, 0, sizeof(*n));
    n->name_off = rt_sint(M, name, nlen);
    n->name_len = nlen;
    n->width    = width;
    n->is_port  = port;
    n->radix    = radix ? radix : TK_RADIX_BIN;
    n->gen      = 1;

    return idx;
}

/* ---- Add cell ---- */

uint32_t
rt_acell(rt_mod_t *M, rt_ctype_t type, uint32_t out,
         const uint32_t *ins, uint8_t n_in, uint32_t width)
{
    uint32_t idx;
    rt_cell_t *c;
    uint8_t i;

    if (M->n_cell >= M->max_cell) return 0;

    idx = M->n_cell++;
    c = &M->cells[idx];
    memset(c, 0, sizeof(*c));
    c->type  = type;
    c->out   = out;
    c->n_in  = n_in > RT_MAX_PIN ? RT_MAX_PIN : n_in;
    c->width = width;
    c->gen   = 1;

    for (i = 0; i < c->n_in; i++)
        c->ins[i] = ins[i];

    /* Set driver on output net */
    if (out > 0 && out < M->n_net)
        M->nets[out].driver = idx;

    return idx;
}

/* ---- Dump RTL for debugging ---- */

void
rt_dump(const rt_mod_t *M)
{
    uint32_t i, j;

    printf("\n--- RTL IR ---\n");
    printf("Nets: %u  Cells: %u\n\n", M->n_net - 1, M->n_cell - 1);

    /* Nets */
    printf("Nets:\n");
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        const char *nm = M->strs + n->name_off;
        const char *dir = "";
        if (n->is_port == 1) dir = " [input]";
        else if (n->is_port == 2) dir = " [output]";
        else if (n->is_port == 3) dir = " [inout]";

        printf("  n%-4u  %*s  w=%-3u%s%s\n",
               i, n->name_len > 20 ? 20 : (int)n->name_len, nm,
               n->width, n->is_reg ? " [reg]" : "", dir);
    }

    /* Cells */
    printf("\nCells:\n");
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        const char *tn;
        if (c->type == RT_CELL_COUNT) continue;  /* dead */
        tn = rt_cname(c->type);

        printf("  c%-4u  %-6s  -> n%-4u  w=%-3u  (",
               i, tn, c->out, c->width);

        for (j = 0; j < c->n_in; j++) {
            if (j > 0) printf(", ");
            printf("n%u", c->ins[j]);
        }
        if (c->type == RT_CONST) {
            printf(") val=%" PRId64, c->param);
        } else {
            printf(")");
        }
        printf("\n");
    }
}
