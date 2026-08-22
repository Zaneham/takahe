/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_rtl.c -- RTL intermediate representation
 *
 * Nets carry signals, cells implement logic, flops hold state, and
 * everything has a width and a driver. This is what the optimiser transforms
 * and the mapper consumes.
 *
 * Two fixed pre-allocated pools, rt_net_t and rt_cell_t. Handles are tagged
 * references carrying generation counters, so use-after-delete returns an
 * error rather than corrupting something quietly.
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
    "MEMRD", "MEMWR", "LUT", "DFFS"
};

/* This table shadows rt_ctype_t and nothing forced them to agree,
 * so adding a cell type used to buy you a read one past the end
 * and a segfault three files away. Now it won't compile. */
typedef char rt_ctn_chk[
    (sizeof(ctnames) / sizeof(ctnames[0]) == RT_CELL_COUNT) ? 1 : -1];

const char *
rt_cname(rt_ctype_t t)
{
    if (t < RT_CELL_COUNT) return ctnames[t];
    return "???";
}

/* ---- Overflow flags ----
 * Process-global. Set the first time a pool rejects an
 * allocation so the diagnostic fires exactly once even
 * though hundreds of subsequent calls also fail. Cleared
 * by rt_init so a follow-on module gets a clean slate. */

static int rt_net_ovf  = 0;
static int rt_cell_ovf = 0;

int rt_ovflow(void) { return rt_net_ovf || rt_cell_ovf; }

/* ---- Init / Free ---- */

int
rt_init(rt_mod_t *M, uint32_t max_net, uint32_t max_cell)
{
    if (!M) return -1;
    memset(M, 0, sizeof(*M));

    /* Fresh module, fresh slate on the overflow flags so the
     * tk_emsg fires again if this module exhausts its pools. */
    rt_net_ovf  = 0;
    rt_cell_ovf = 0;

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

/* ---- Add net (source-tagged) ----
 * The real function. Untagged rt_anet below is the thin
 * wrapper that passes zero, which is fine for callers that
 * have nothing to say about provenance. */

uint32_t
rt_anet_at(rt_mod_t *M, const char *name, uint16_t nlen,
           uint32_t width, uint8_t port, uint8_t radix,
           uint32_t line, uint16_t col)
{
    uint32_t idx;
    rt_net_t *n;

    if (M->n_net >= M->max_net) {
        if (!rt_net_ovf) {
            /* TK020: net pool exhausted (n nets, max m). */
            tk_emsg(20, M->n_net, M->max_net);
            rt_net_ovf = 1;
        }
        return 0;
    }

    idx = M->n_net++;
    n = &M->nets[idx];
    memset(n, 0, sizeof(*n));
    n->name_off = rt_sint(M, name, nlen);
    n->name_len = nlen;
    n->width    = width;
    n->is_port  = port;
    n->radix    = radix ? radix : TK_RADIX_BIN;
    n->gen      = 1;
    n->line     = line;
    n->col      = col;

    return idx;
}

uint32_t
rt_anet(rt_mod_t *M, const char *name, uint16_t nlen,
        uint32_t width, uint8_t port, uint8_t radix)
{
    return rt_anet_at(M, name, nlen, width, port, radix, 0, 0);
}

/* ---- Add cell (source-tagged) ---- */

uint32_t
rt_acell_at(rt_mod_t *M, rt_ctype_t type, uint32_t out,
            const uint32_t *ins, uint8_t n_in, uint32_t width,
            uint32_t line, uint16_t col)
{
    uint32_t idx;
    rt_cell_t *c;
    uint8_t i;

    if (M->n_cell >= M->max_cell) {
        if (!rt_cell_ovf) {
            /* TK021: cell pool exhausted (n cells, max m). */
            tk_emsg(21, M->n_cell, M->max_cell);
            rt_cell_ovf = 1;
        }
        return 0;
    }

    idx = M->n_cell++;
    c = &M->cells[idx];
    memset(c, 0, sizeof(*c));
    c->type  = type;
    c->out   = out;
    c->n_in  = n_in > RT_MAX_PIN ? RT_MAX_PIN : n_in;
    c->width = width;
    c->gen   = 1;
    c->line  = line;
    c->col   = col;

    for (i = 0; i < c->n_in; i++)
        c->ins[i] = ins[i];

    /* Set driver on output net */
    if (out > 0 && out < M->n_net)
        M->nets[out].driver = idx;

    return idx;
}

uint32_t
rt_acell(rt_mod_t *M, rt_ctype_t type, uint32_t out,
         const uint32_t *ins, uint8_t n_in, uint32_t width)
{
    return rt_acell_at(M, type, out, ins, n_in, width, 0, 0);
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

uint32_t
rt_undrv(const rt_mod_t *M)
{
    uint32_t i, j, n = 0;
    uint8_t *used, *drvn;

    if (!M || M->n_net == 0) return 0;

    used = (uint8_t *)calloc(M->n_net, 1);
    drvn = (uint8_t *)calloc(M->n_net, 1);
    if (!used || !drvn) { free(used); free(drvn); return 0; }

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->out > 0 && c->out < M->n_net) drvn[c->out] = 1;
        for (j = 0; j < c->n_in; j++)
            if (c->ins[j] > 0 && c->ins[j] < M->n_net)
                used[c->ins[j]] = 1;
    }

    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *nt = &M->nets[i];
        if (!used[i] || drvn[i]) continue;
        if (nt->driver != 0) continue;
        if (nt->is_port == 1 || nt->is_port == 3) continue;
        tk_emsg(23, M->strs + nt->name_off, nt->line, (unsigned)nt->col);
        n++;
    }

    free(used);
    free(drvn);
    return n;
}

rt_mod_t *
rt_mclone(const rt_mod_t *M)
{
    rt_mod_t *D;

    if (!M) return NULL;
    D = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    if (!D) return NULL;

    memcpy(D, M, sizeof(rt_mod_t));
    D->nets  = (rt_net_t *)malloc(M->max_net * sizeof(rt_net_t));
    D->cells = (rt_cell_t *)malloc(M->max_cell * sizeof(rt_cell_t));
    D->strs  = (char *)malloc(M->str_max);
    if (!D->nets || !D->cells || !D->strs) {
        free(D->nets); free(D->cells); free(D->strs); free(D);
        return NULL;
    }
    memcpy(D->nets,  M->nets,  M->n_net  * sizeof(rt_net_t));
    memcpy(D->cells, M->cells, M->n_cell * sizeof(rt_cell_t));
    memcpy(D->strs,  M->strs,  M->str_len);
    return D;
}

int
rt_split(const rt_mod_t *M, uint32_t ni)
{
    char probe[64];
    int pl;
    uint32_t k;

    if (!M || ni == 0 || ni >= M->n_net) return 0;
    if (M->nets[ni].width <= 1) return 0;
    pl = snprintf(probe, sizeof(probe), "%.*s_0",
                  (int)M->nets[ni].name_len,
                  M->strs + M->nets[ni].name_off);
    if (pl < 0 || pl >= (int)sizeof(probe)) return 0;
    for (k = 1; k < M->n_net; k++) {
        if (M->nets[k].name_len == (uint16_t)pl &&
            memcmp(M->strs + M->nets[k].name_off, probe, (size_t)pl) == 0 &&
            M->nets[k].is_port == M->nets[ni].is_port)
            return 1;
    }
    return 0;
}
