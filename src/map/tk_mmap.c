/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_mmap.c -- memory primitive library loader and matcher
 *
 * Reads defs/mems_<family>.def into an ml_lib_t, then walks rt_mod_t.mems[]
 * picking the cheapest primitive that fits each inferred memory. Adding an
 * FPGA or a PDK is a text file, not a recompile.
 *
 * No flop-forest fallback. A memory that fits nothing keeps its RT_MEMRD and
 * RT_MEMWR cells and the existing emitters handle it as before, because
 * leaving it alone beats rewriting it wrong.
 */

#include "takahe.h"

#define ML_LINE_MAX  256
#define ML_TOK_MAX    40

/* ---- Tiny tokeniser ----
 * Splits one line into whitespace-delimited tokens. Caller
 * supplies the buffer for the line and the storage for the tokens,
 * and this points into that buffer with NUL terminators.
 * Comments (#) and trailing newlines are stripped. Bounded
 * everywhere, no allocations. */

static int
ml_toks(char *line, char *toks[], int max)
{
    char *p = line;
    int n = 0;

    KA_GUARD(gtk, ML_LINE_MAX);
    while (*p && gtk--) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '\0' || *p == '#') break;
        if (n >= max) break;
        toks[n++] = p;
        /* Walk to end of token */
        while (*p && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n' && *p != '#')
            p++;
        if (*p) { *p = '\0'; p++; }
    }
    return n;
}

/* ---- Keyword recognition ----
 * Simple linear strcmp dispatch. The keyword set is small
 * (under thirty) and the parser runs once at startup, so a
 * hash table would be overkill and a perfect-hash generator
 * would be vanity. */

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* ---- Parse a port kind keyword ---- */

static int
ml_pkind(const char *s)
{
    if (eq(s, "ar"))   return ML_PORT_AR;
    if (eq(s, "sr"))   return ML_PORT_SR;
    if (eq(s, "aw"))   return ML_PORT_AW;
    if (eq(s, "sw"))   return ML_PORT_SW;
    if (eq(s, "arsw")) return ML_PORT_ARSW;
    if (eq(s, "srsw")) return ML_PORT_SRSW;
    return -1;
}

/* ---- Parse a prim type keyword ---- */

static int
ml_ptype(const char *s)
{
    if (eq(s, "bram"))   return ML_TYPE_BRAM;
    if (eq(s, "sram"))   return ML_TYPE_SRAM;
    if (eq(s, "lutram")) return ML_TYPE_LUTRAM;
    if (eq(s, "spram"))  return ML_TYPE_SPRAM;
    return -1;
}

/* ---- Bounded copy into a small buffer ---- */

static void
ml_scpy(char *dst, int dsz, const char *src)
{
    int n = (int)strlen(src);
    if (n >= dsz) n = dsz - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* ---- Public: load one .def file ---- */

int
ml_load(ml_lib_t *lib, const char *path)
{
    FILE *fp;
    char line[ML_LINE_MAX];
    char *toks[ML_TOK_MAX];
    ml_prim_t *cur = NULL;
    ml_port_t *port = NULL;
    int nt;

    if (!lib || !path) return -1;

    fp = fopen(path, "r");
    if (!fp) {
        /* TK001 covers this. Caller decides whether the
         * absence of a memory library is fatal. */
        tk_emsg(1, path);
        return -1;
    }

    KA_GUARD(gln, 100000);
    while (fgets(line, ML_LINE_MAX, fp) && gln--) {
        nt = ml_toks(line, toks, ML_TOK_MAX);
        if (nt == 0) continue;

        /* ---- "end" closes the current scope ---- */
        if (eq(toks[0], "end")) {
            if (port) {
                port = NULL;
            } else if (cur) {
                cur = NULL;
            }
            continue;
        }

        /* ---- "mem <NAME>" opens a primitive ---- */
        if (eq(toks[0], "mem") && nt >= 2 && !cur) {
            if (lib->n_prim >= ML_MAX_PRIMS) {
                /* Silent overflow: pools are sized at compile
                 * time, exceeding them is the user's call. */
                continue;
            }
            cur = &lib->prims[lib->n_prim++];
            memset(cur, 0, sizeof(*cur));
            ml_scpy(cur->name, ML_NAME_LEN, toks[1]);
            cur->cost = 100;  /* default */
            continue;
        }

        if (!cur) continue;  /* outside any mem block */

        /* ---- "port <kind> <name>" opens a port ---- */
        if (eq(toks[0], "port") && nt >= 3 && !port) {
            int k = ml_pkind(toks[1]);
            if (k < 0) continue;
            if (cur->n_ports >= ML_MAX_PORTS) continue;
            port = &cur->ports[cur->n_ports++];
            memset(port, 0, sizeof(*port));
            port->kind = (uint8_t)k;
            ml_scpy(port->name, sizeof(port->name), toks[2]);
            continue;
        }

        /* ---- Inside a port: port-level attributes ---- */
        if (port) {
            if (eq(toks[0], "clock") && nt >= 2) {
                if (eq(toks[1], "posedge"))      port->clk = 1;
                else if (eq(toks[1], "negedge")) port->clk = 2;
                else                              port->clk = 0;
            } else if (eq(toks[0], "has_we"))      port->has_we = 1;
            else if (eq(toks[0], "has_re"))        port->has_re = 1;
            else if (eq(toks[0], "has_clken"))     port->has_clken = 1;
            else if (eq(toks[0], "active_low_en")) port->active_low = 1;
            else if (eq(toks[0], "has_be") && nt >= 2)
                port->be_gran = (uint8_t)atoi(toks[1]);
            else if (eq(toks[0], "addr_bswap") && nt >= 2)
                port->abswap = (uint8_t)atoi(toks[1]);
            continue;
        }

        /* ---- Inside a mem but outside a port: header attrs ---- */
        if (eq(toks[0], "type") && nt >= 2) {
            int t = ml_ptype(toks[1]);
            if (t >= 0) cur->type = (uint8_t)t;
        } else if (eq(toks[0], "abits") && nt >= 2) {
            cur->abits = (uint8_t)atoi(toks[1]);
        } else if (eq(toks[0], "widths") && nt >= 2) {
            int i;
            cur->n_widths = 0;
            for (i = 1; i < nt && cur->n_widths < ML_MAX_WIDTHS; i++)
                cur->widths[cur->n_widths++] = (uint8_t)atoi(toks[i]);
        } else if (eq(toks[0], "size") && nt >= 2) {
            cur->size = (uint32_t)atol(toks[1]);
        } else if (eq(toks[0], "cost") && nt >= 2) {
            cur->cost = (uint16_t)atoi(toks[1]);
        } else if (eq(toks[0], "init") && nt >= 2) {
            if (eq(toks[1], "none"))      cur->init = 0;
            else if (eq(toks[1], "zero")) cur->init = 1;
            else if (eq(toks[1], "any"))  cur->init = 2;
        }
    }

    fclose(fp);
    printf("takahe: memlib: %u primitives loaded from %s\n",
           (unsigned)lib->n_prim, path);
    return 0;
}

/* ---- Matcher ----
 * Does primitive p fit memory m? "Fit" today is the simplest
 * possible question: can the data width be supported (one of
 * p->widths) and is the depth at most 2^p->abits at that
 * width. Tiling, dual port, transparency, byte-enable
 * alignment, init-data shaping all come later. */

static int
mm_fit(const ml_prim_t *p, uint32_t data_w, uint32_t depth)
{
    int i;
    uint32_t tile_depth;

    if (data_w == 0 || depth == 0) return 0;

    /* Need at least one supported width that is >= data_w.
     * Same width is best; wider wastes bits but still fits. */
    for (i = 0; i < p->n_widths; i++) {
        if (p->widths[i] < data_w) continue;
        /* Depth at this width = total bits / chosen width */
        tile_depth = p->size / p->widths[i];
        if (depth <= tile_depth) return 1;
    }
    return 0;
}

/* Pick the lowest-cost primitive that fits, or -1 if none. */

static int
mm_pick(const ml_lib_t *lib, uint32_t data_w, uint32_t depth)
{
    int i, best = -1;
    uint16_t best_cost = 0xffff;

    for (i = 0; i < lib->n_prim; i++) {
        if (!mm_fit(&lib->prims[i], data_w, depth)) continue;
        if (lib->prims[i].cost < best_cost) {
            best_cost = lib->prims[i].cost;
            best = i;
        }
    }
    return best;
}

/* ---- Public: map a module's memories against a library ---- */

int
mp_mmap(rt_mod_t *M, const ml_lib_t *lib)
{
    uint32_t i;
    int mapped = 0;

    if (!M || !lib) return 0;

    for (i = 0; i < M->n_mem; i++) {
        int p = mm_pick(lib, M->mems[i].data_w, M->mems[i].depth);
        if (p >= 0) {
            M->mems[i].prim_idx = (uint8_t)p;
            M->mems[i].prim_set = 1;
            mapped++;
            printf("takahe: memlib: mem '%.*s' (%ux%u) -> %s\n",
                   (int)M->mems[i].name_len,
                   M->strs + M->mems[i].name_off,
                   M->mems[i].data_w, M->mems[i].depth,
                   lib->prims[p].name);
        } else {
            printf("takahe: memlib: mem '%.*s' (%ux%u) no match, "
                   "leaving as soft logic\n",
                   (int)M->mems[i].name_len,
                   M->strs + M->mems[i].name_off,
                   M->mems[i].data_w, M->mems[i].depth);
        }
    }
    return mapped;
}
