/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_flat.c -- Hierarchy flattening for Takahe
 *
 * Inlines module instantiations into a single flat design.
 * After this pass, there are no INSTANCE nodes — every gate,
 * wire, and always block lives in one namespace with prefixed
 * names to avoid collisions.
 *
 * Why flatten? Synthesis operates on a flat netlist. The
 * hierarchy is useful for humans reading the code, but the
 * mapper doesn't care which module a gate came from — it
 * just needs to know what's connected to what.
 *
 * Algorithm:
 *   1. Build a module table (name -> AST node)
 *   2. For each INSTANCE node in the top module:
 *      a. Look up the instantiated module definition
 *      b. Copy its body into the parent, prefixing all
 *         signal names with "instname_"
 *      c. Wire port connections to the prefixed names
 *      d. Mark the INSTANCE node as dead
 *   3. Repeat until no INSTANCE nodes remain (bounded)
 *
 * Like unpacking Russian dolls: each inner doll's contents
 * get spread across the table with a label saying which
 * doll they came from.
 *
 * NOTE: For Tier 1, we do a simplified version. Full
 * flattening with signal renaming and port binding is
 * complex. Instead, we build the module lookup table
 * and annotate instances with their definition index.
 * The RTL lowerer (Tier 2) will use this annotation to
 * process each module independently and wire them up.
 *
 * JPL Power of 10: bounded, no alloc, fixed tables.
 */

#include "takahe.h"

/* ---- Module Table ---- */

#define FL_MAX_MOD  64

typedef struct {
    uint32_t name_off;   /* string pool offset */
    uint16_t name_len;
    uint32_t node;       /* AST node index of MODULE */
} fl_mod_t;

typedef struct {
    fl_mod_t  mods[FL_MAX_MOD];
    uint32_t  n_mod;
    tk_parse_t *P;
} fl_ctx_t;

/* ---- Collect module definitions ---- */

static void
fl_cmods(fl_ctx_t *F, uint32_t node)
{
    uint32_t c;

    if (node == 0 || KA_CHK(node, F->P->n_node)) return;

    if (F->P->nodes[node].type == TK_AST_MODULE) {
        if (F->n_mod < FL_MAX_MOD) {
            fl_mod_t *m = &F->mods[F->n_mod];
            m->name_off = F->P->nodes[node].d.text.off;
            m->name_len = F->P->nodes[node].d.text.len;
            m->node     = node;
            F->n_mod++;
        }
    }

    /* Only scan top-level children (modules are at root level) */
    c = F->P->nodes[node].first_child;
    KA_GUARD(g, 10000);
    while (c && g--) {
        /* Don't recurse into modules — they don't nest in SV */
        if (F->P->nodes[c].type == TK_AST_MODULE) {
            if (F->n_mod < FL_MAX_MOD) {
                fl_mod_t *m = &F->mods[F->n_mod];
                m->name_off = F->P->nodes[c].d.text.off;
                m->name_len = F->P->nodes[c].d.text.len;
                m->node     = c;
                F->n_mod++;
            }
        }
        c = F->P->nodes[c].next_sib;
    }
}

/* ---- Find module by name ---- */

static int
fl_fmod(const fl_ctx_t *F, const char *name, uint16_t nlen)
{
    uint32_t i;
    for (i = 0; i < F->n_mod; i++) {
        if (F->mods[i].name_len == nlen &&
            memcmp(F->P->lex->strs + F->mods[i].name_off,
                   name, nlen) == 0)
            return (int)i;
    }
    return -1;
}

/* ---- Annotate instances with module definition ----
 * Walk the AST and for each INSTANCE node, look up the
 * module name in the table. Store the module table index
 * in the instance node's op field. This lets the RTL
 * lowerer find the definition without searching again.
 *
 * Also count unresolved instances (modules not defined
 * in this file — they're external and will be treated
 * as black boxes). */

static int
fl_annot(fl_ctx_t *F, uint32_t node)
{
    uint32_t c;
    int resolved = 0;

    if (node == 0 || KA_CHK(node, F->P->n_node)) return 0;

    /* Skip dead nodes */
    if (F->P->nodes[node].type == TK_AST_COUNT) return 0;

    if (F->P->nodes[node].type == TK_AST_INSTANCE) {
        const char *mname = F->P->lex->strs +
            F->P->nodes[node].d.text.off;
        uint16_t mlen = F->P->nodes[node].d.text.len;

        int mi = fl_fmod(F, mname, mlen);
        if (mi >= 0) {
            /* Found! Store module index in op field.
             * Add 1 to distinguish from "not set" (0). */
            F->P->nodes[node].op = (uint16_t)(mi + 1);
            resolved++;
        }
        /* If not found, op stays 0 = external/black box.
         * Like an import declaration: we know it exists,
         * we just can't see inside. */
    }

    /* Recurse */
    c = F->P->nodes[node].first_child;
    KA_GUARD(g, 100000);
    while (c && g--) {
        resolved += fl_annot(F, c);
        c = F->P->nodes[c].next_sib;
    }

    return resolved;
}

/* ---- Count instances ---- */

static void
fl_stats(const fl_ctx_t *F, uint32_t node,
         int *n_inst, int *n_ext)
{
    uint32_t c;

    if (node == 0 || KA_CHK(node, F->P->n_node)) return;
    if (F->P->nodes[node].type == TK_AST_COUNT) return;

    if (F->P->nodes[node].type == TK_AST_INSTANCE) {
        (*n_inst)++;
        if (F->P->nodes[node].op == 0)
            (*n_ext)++;
    }

    c = F->P->nodes[node].first_child;
    KA_GUARD(g, 100000);
    while (c && g--) {
        fl_stats(F, c, n_inst, n_ext);
        c = F->P->nodes[c].next_sib;
    }
}

/* ---- Public API ---- */

int
fl_flat(tk_parse_t *P)
{
    fl_ctx_t F;
    int n_inst = 0, n_ext = 0;

    if (!P) return -1;

    memset(&F, 0, sizeof(F));
    F.P = P;

    /* Collect module definitions */
    fl_cmods(&F, 1);
    printf("takahe: flat: %u module definitions found\n", F.n_mod);
    {
        uint32_t i;
        for (i = 0; i < F.n_mod; i++) {
            printf("  [%u] %.*s (node %u)\n",
                   i,
                   (int)F.mods[i].name_len,
                   P->lex->strs + F.mods[i].name_off,
                   F.mods[i].node);
        }
    }

    /* Annotate instances with module definitions */
    fl_annot(&F, 1);

    /* Count instances */
    fl_stats(&F, 1, &n_inst, &n_ext);
    printf("takahe: flat: %d instances (%d external/blackbox)\n",
           n_inst, n_ext);

    return 0;
}
