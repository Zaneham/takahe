/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_elab.c -- SystemVerilog elaboration for Takahe
 *
 * Resolves parameters, expands generates, infers widths.
 * Turns a syntactic AST into a semantically resolved AST
 * ready for RTL lowering.
 *
 * Three passes:
 *   1. Collect parameters and evaluate defaults (ce_eval)
 *   2. Substitute parameter values into expressions
 *   3. Evaluate generate conditions, expand/prune branches
 *
 * Like customs processing at the border: everything gets
 * inspected, stamped, and either admitted or turned away.
 * Parameters are the passports. Generate blocks are the
 * conditional entry lanes.
 *
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Parameter Table ---- */

#define EL_MAX_PAR  512    /* max parameters across all modules */

typedef struct {
    uint32_t name_off;    /* string pool offset of param name */
    uint16_t name_len;
    ce_val_t val;          /* resolved value */
    uint32_t mod_node;    /* which module owns this param */
} el_par_t;

typedef struct {
    el_par_t  pars[EL_MAX_PAR];
    uint32_t  n_par;

    /* Reference to parser for AST + string pool access */
    tk_parse_t *P;
    ce_val_t   *cvals;     /* constant values per node */
    uint32_t    nvals;
} el_ctx_t;

/* ---- Helpers ---- */

/* Find parameter by name in the table */
static int
el_fpar(const el_ctx_t *E, const char *name, uint16_t nlen)
{
    uint32_t i;
    for (i = 0; i < E->n_par; i++) {
        if (E->pars[i].name_len == nlen &&
            memcmp(E->P->lex->strs + E->pars[i].name_off,
                   name, nlen) == 0)
            return (int)i;
    }
    return -1;
}

/* ---- Pass 1: Collect parameters ----
 * Walk the AST looking for PARAM and LOCALPARAM nodes.
 * Evaluate their default values using the constant evaluator.
 * Store name -> value mappings in the parameter table. */

static void
el_cpar(el_ctx_t *E, uint32_t node)
{
    uint32_t c, name_c, val_c;
    tk_node_t *n;

    if (node == 0 || KA_CHK(node, E->P->n_node)) return;
    n = &E->P->nodes[node];

    if (n->type == TK_AST_PARAM || n->type == TK_AST_LOCALPARAM) {
        /* Find the IDENT child (param name) and value child */
        name_c = 0;
        val_c = 0;
        c = n->first_child;
        KA_GUARD(g, 100);
        while (c && g--) {
            tk_node_t *cn = &E->P->nodes[c];
            if (cn->type == TK_AST_IDENT && name_c == 0)
                name_c = c;
            else if (cn->type != TK_AST_TYPE_SPEC &&
                     cn->type != TK_AST_RANGE &&
                     cn->type != TK_AST_IDENT)
                val_c = c;  /* value expression */
            c = cn->next_sib;
        }

        if (name_c && E->n_par < EL_MAX_PAR) {
            el_par_t *p = &E->pars[E->n_par];
            p->name_off = E->P->nodes[name_c].d.text.off;
            p->name_len = E->P->nodes[name_c].d.text.len;
            p->mod_node = 0; /* TODO: track module */

            /* Try to evaluate the default value */
            if (val_c && val_c < E->nvals && E->cvals[val_c].valid) {
                p->val = E->cvals[val_c];
            } else {
                p->val.valid = 0;
            }

            E->n_par++;
        }
    }

    /* Recurse into children */
    c = n->first_child;
    KA_GUARD(g2, 100000);
    while (c && g2--) {
        el_cpar(E, c);
        c = E->P->nodes[c].next_sib;
    }
}

/* ---- Pass 2: Substitute parameter values ----
 * Walk the AST looking for IDENT nodes that match a parameter
 * name. Mark their cvals entry with the parameter's value.
 * Then re-run ce_eval to propagate the new constants. */

static void
el_subst(el_ctx_t *E, uint32_t node)
{
    uint32_t c;
    tk_node_t *n;

    if (node == 0 || KA_CHK(node, E->P->n_node)) return;
    n = &E->P->nodes[node];

    if (n->type == TK_AST_IDENT && n->d.text.len > 0) {
        const char *name = E->P->lex->strs + n->d.text.off;
        int pi = el_fpar(E, name, n->d.text.len);
        if (pi >= 0 && E->pars[pi].val.valid) {
            /* Stamp the parameter value onto this ident node */
            if (node < E->nvals) {
                E->cvals[node] = E->pars[pi].val;
            }
        }
    }

    /* Recurse */
    c = n->first_child;
    KA_GUARD(g, 100000);
    while (c && g--) {
        el_subst(E, c);
        c = E->P->nodes[c].next_sib;
    }
}

/* ---- Pass 3: Evaluate generate conditions ----
 * For generate-if blocks, check if the condition is now
 * a resolved constant. If true, keep the true branch and
 * remove the false branch. If false, do the opposite.
 * If unknown, keep both (will error later).
 *
 * For now, we just annotate — actual pruning comes when
 * we have the RTL lowering stage to skip dead branches. */

static void
el_geval(el_ctx_t *E, uint32_t node)
{
    uint32_t c;
    tk_node_t *n;

    if (node == 0 || KA_CHK(node, E->P->n_node)) return;
    n = &E->P->nodes[node];

    if (n->type == TK_AST_IF) {
        /* First child is the condition expression */
        uint32_t cond = n->first_child;
        if (cond && cond < E->nvals && E->cvals[cond].valid) {
            /* Condition resolved! Store result in the IF node's
             * op field: 1 = false, 2 = true. 0 means "not
             * evaluated" — runtime conditional, hands off.
             * ge_prune only prunes op==1 or op==2 nodes. */
            n->op = (uint16_t)(E->cvals[cond].val != 0 ? 2 : 1);
        }
    }

    /* Recurse */
    c = n->first_child;
    KA_GUARD(g, 100000);
    while (c && g--) {
        el_geval(E, c);
        c = E->P->nodes[c].next_sib;
    }
}

/* ---- Public API ---- */

int
el_elab(tk_parse_t *P, ce_val_t *cvals, uint32_t nvals)
{
    el_ctx_t E;

    if (!P || !cvals) return -1;

    memset(&E, 0, sizeof(E));
    E.P     = P;
    E.cvals = cvals;
    E.nvals = nvals;

    /* Pass 1: collect parameters and their default values */
    el_cpar(&E, 1);  /* node 1 = ROOT */
    printf("takahe: elab: %u parameters collected\n", E.n_par);

    /* Debug: show collected params */
    {
        uint32_t i;
        for (i = 0; i < E.n_par; i++) {
            const char *nm = P->lex->strs + E.pars[i].name_off;
            if (E.pars[i].val.valid) {
                printf("  %.*s = %" PRId64 "\n",
                       (int)E.pars[i].name_len, nm,
                       E.pars[i].val.val);
            } else {
                printf("  %.*s = <unresolved>\n",
                       (int)E.pars[i].name_len, nm);
            }
        }
    }

    /* Pass 2: substitute parameter values into ident nodes */
    el_subst(&E, 1);

    /* Re-evaluate constants with parameter values now known */
    {
        int more = ce_eval(P, cvals, nvals);
        printf("takahe: elab: %d additional constants after param subst\n",
               more);
    }

    /* Pass 2b: re-collect localparams that depend on params.
     * ADDR_W = $clog2(DEPTH) only resolves after DEPTH is
     * substituted and re-evaluated. So we collect again. */
    E.n_par = 0;
    el_cpar(&E, 1);

    /* Substitute again with updated values */
    el_subst(&E, 1);
    ce_eval(P, cvals, nvals);

    printf("takahe: elab: %u parameters after second pass\n", E.n_par);
    {
        uint32_t i;
        for (i = 0; i < E.n_par; i++) {
            const char *nm = P->lex->strs + E.pars[i].name_off;
            if (E.pars[i].val.valid) {
                printf("  %.*s = %" PRId64 "\n",
                       (int)E.pars[i].name_len, nm,
                       E.pars[i].val.val);
            } else {
                printf("  %.*s = <unresolved>\n",
                       (int)E.pars[i].name_len, nm);
            }
        }
    }

    /* Pass 3: evaluate generate conditions */
    el_geval(&E, 1);

    return 0;
}
