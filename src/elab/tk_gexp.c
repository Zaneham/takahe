/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_gexp.c -- Generate block expansion for Takahe
 *
 * Prunes dead branches from generate-if blocks and unrolls
 * generate-for loops. After elaboration, the IF nodes have
 * their conditions evaluated (el_geval stamped the result
 * into n->op). This pass walks the AST and:
 *
 *   1. For generate-if with resolved condition:
 *      - Keep the true branch, mark the false branch dead
 *      - Or vice versa if condition is false
 *
 *   2. For generate-for (future):
 *      - Unroll the loop body N times
 *      - Each copy gets the loop variable substituted
 *
 * Dead nodes are marked by setting their type to TK_AST_COUNT
 * (the sentinel value). Downstream passes skip them. Like
 * decommissioning a reactor: the building stays, but nothing
 * inside works anymore.
 *
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Mark a subtree as dead ----
 * Sets all nodes in the subtree to TK_AST_COUNT (sentinel).
 * Iterative with explicit stack to avoid recursion. */

#define GE_STSZ  256  /* dead-marking stack depth */

static void
ge_kill(tk_parse_t *P, uint32_t root)
{
    uint32_t stk[GE_STSZ];
    int sp = 0;
    uint32_t n;

    if (root == 0 || KA_CHK(root, P->n_node)) return;

    stk[sp++] = root;

    /* Iterative tree walk. The explicit stack is bounded by
     * GE_STSZ which limits nesting depth. If someone writes
     * a 256-deep nested generate, they deserve what happens,
     * but we won't crash — we'll just stop pruning. */
    KA_GUARD(g, 100000);
    while (sp > 0 && g--) {
        n = stk[--sp];
        if (n == 0 || KA_CHK(n, P->n_node)) continue;

        /* Mark dead */
        P->nodes[n].type = TK_AST_COUNT;

        /* Push children */
        {
            uint32_t c = P->nodes[n].first_child;
            KA_GUARD(gc, 10000);
            while (c && gc--) {
                if (sp < GE_STSZ)
                    stk[sp++] = c;
                c = P->nodes[c].next_sib;
            }
        }
    }
}

/* ---- Prune generate-if blocks ----
 * Walk the AST looking for IF nodes whose op field was set
 * by el_geval (0 = false, 1 = true). For each:
 *   - If true: keep first body child, kill else branch
 *   - If false: keep else branch, kill first body child
 *
 * An IF node has children: [condition, then_body, else_body?]
 * The condition was child 0, then_body child 1, else_body child 2.
 * If there's no else, child 2 doesn't exist. */

static int
ge_prune(tk_parse_t *P, uint32_t node)
{
    uint32_t c;
    int pruned = 0;

    if (node == 0 || KA_CHK(node, P->n_node)) return 0;

    /* Skip dead nodes */
    if (P->nodes[node].type == TK_AST_COUNT) return 0;

    if (P->nodes[node].type == TK_AST_IF) {
        uint16_t resolved = P->nodes[node].op;

        /* op == 0 means "not evaluated" (runtime conditional)
         * op == 1 means "condition evaluated to false"
         * op == 2 means "condition evaluated to true"
         * Only generate-time IFs get stamped by el_geval.
         * Runtime IFs stay at op==0 and we leave them alone. */
        if (resolved == 1 || resolved == 2) {
            uint32_t cond, then_b, else_b;

            cond = P->nodes[node].first_child;
            if (cond == 0) goto recurse;

            then_b = P->nodes[cond].next_sib;
            if (then_b == 0) goto recurse;

            else_b = P->nodes[then_b].next_sib;

            if (resolved == 2) {
                /* Condition true: kill else branch if present */
                if (else_b) {
                    ge_kill(P, else_b);
                    P->nodes[then_b].next_sib = 0;
                    pruned++;
                }
            } else {
                /* Condition false: kill then branch */
                ge_kill(P, then_b);
                if (else_b) {
                    /* Promote else to then position */
                    P->nodes[cond].next_sib = else_b;
                } else {
                    /* No else: kill the whole IF, it's dead.
                     * Like a traffic light with no green phase:
                     * technically present, functionally useless. */
                    ge_kill(P, node);
                }
                pruned++;
            }
        }
    }

recurse:
    /* Recurse into live children */
    c = P->nodes[node].first_child;
    KA_GUARD(g, 100000);
    while (c && g--) {
        if (P->nodes[c].type != TK_AST_COUNT)
            pruned += ge_prune(P, c);
        c = P->nodes[c].next_sib;
    }

    return pruned;
}

/* ---- Generate-for unrolling ----
 * Clones the loop body N times, substituting the genvar
 * with concrete integer values. Each copy gets unique names
 * by appending the iteration index to identifiers.
 *
 * FOR children: [init_assign, condition, incr_assign, body]
 * init_assign: BLOCK_ASSIGN(IDENT 'i', INT_LIT '0')
 * condition:   BINOP '<'(IDENT 'i', INT_LIT '4')
 * body:        BEGIN_END with instances/assigns */

/* Clone a subtree starting at `src`, substituting IDENT nodes
 * matching `gvar` (name_off/name_len) with INT_LIT of value `val`.
 * Returns the root of the cloned subtree. */

static uint32_t
ge_clone(tk_parse_t *P, uint32_t src, uint32_t gvar_off,
         uint16_t gvar_len, int64_t val)
{
    uint32_t dst, ch, prev_ch;

    if (src == 0 || KA_CHK(src, P->n_node)) return 0;
    if (P->n_node >= P->max_node) return 0;

    /* Allocate new node */
    dst = P->n_node++;
    memcpy(&P->nodes[dst], &P->nodes[src], sizeof(tk_node_t));
    P->nodes[dst].first_child = 0;
    P->nodes[dst].last_child  = 0;
    P->nodes[dst].next_sib    = 0;

    /* If this IDENT matches the genvar, replace with INT_LIT */
    if (P->nodes[src].type == TK_AST_IDENT &&
        P->nodes[src].d.text.len == gvar_len) {
        const char *nm = P->lex->strs + P->nodes[src].d.text.off;
        if (memcmp(nm, P->lex->strs + gvar_off, gvar_len) == 0) {
            /* Replace with integer literal */
            char buf[16];
            int blen = snprintf(buf, sizeof(buf), "%" PRId64, val);
            tk_lex_t *ml = (tk_lex_t *)P->lex;
            if (blen > 0 && ml->str_len + (uint32_t)blen + 1 <= ml->str_max) {
                uint32_t off = ml->str_len;
                memcpy(ml->strs + off, buf, (size_t)blen);
                ml->strs[off + (uint32_t)blen] = '\0';
                ml->str_len += (uint32_t)blen + 1;
                P->nodes[dst].type = TK_AST_INT_LIT;
                P->nodes[dst].d.text.off = off;
                P->nodes[dst].d.text.len = (uint16_t)blen;
            }
            return dst;
        }
    }

    /* Clone children */
    ch = P->nodes[src].first_child;
    prev_ch = 0;
    KA_GUARD(g, 10000);
    while (ch && g--) {
        uint32_t cln = ge_clone(P, ch, gvar_off, gvar_len, val);
        if (cln == 0) break;
        cln = cln; /* suppress warning */
        P->nodes[cln].next_sib = 0;
        if (P->nodes[dst].first_child == 0) {
            P->nodes[dst].first_child = cln;
        } else {
            P->nodes[prev_ch].next_sib = cln;
        }
        P->nodes[dst].last_child = cln;
        prev_ch = cln;
        ch = P->nodes[ch].next_sib;
    }

    return dst;
}

/* Unroll a single FOR node inside a GENERATE block.
 * Extracts loop bounds, clones body N times, replaces
 * the FOR with the cloned bodies. Returns count unrolled. */

static int
ge_unfor(tk_parse_t *P, uint32_t for_n, uint32_t parent)
{
    uint32_t init_n, cond_n, incr_n, body_n;
    uint32_t gvar_off;
    uint16_t gvar_len;
    int64_t lo, hi;
    int unrolled = 0;

    if (for_n == 0 || KA_CHK(for_n, P->n_node)) return 0;

    /* FOR children: init, condition, increment, body */
    init_n = P->nodes[for_n].first_child;
    if (!init_n) return 0;
    cond_n = P->nodes[init_n].next_sib;
    if (!cond_n) return 0;
    incr_n = P->nodes[cond_n].next_sib;
    if (!incr_n) return 0;
    body_n = P->nodes[incr_n].next_sib;
    if (!body_n) return 0;

    /* Extract genvar name from init: BLOCK_ASSIGN(IDENT, INT_LIT) */
    {
        uint32_t gv = P->nodes[init_n].first_child;
        uint32_t iv;
        if (!gv || P->nodes[gv].type != TK_AST_IDENT) return 0;
        gvar_off = P->nodes[gv].d.text.off;
        gvar_len = P->nodes[gv].d.text.len;

        /* Init value */
        iv = P->nodes[gv].next_sib;
        if (!iv) return 0;
        {
            const char *s = P->lex->strs + P->nodes[iv].d.text.off;
            lo = (int64_t)atoll(s);
        }
    }

    /* Extract upper bound from condition: BINOP '<'(IDENT, INT_LIT) */
    {
        uint32_t rhs = P->nodes[cond_n].first_child;
        if (rhs) rhs = P->nodes[rhs].next_sib;
        if (!rhs) return 0;
        {
            const char *s = P->lex->strs + P->nodes[rhs].d.text.off;
            hi = (int64_t)atoll(s);
        }
    }

    if (hi <= lo || hi - lo > 256) return 0; /* sanity */

    printf("takahe: gexp: unrolling for %.*s = %" PRId64 "..%" PRId64 "\n",
           (int)gvar_len, P->lex->strs + gvar_off, lo, hi - 1);

    /* Clone body for each iteration, attach as siblings after FOR.
     * Then kill the FOR node itself. */
    {
        int64_t iter;
        uint32_t prev = for_n;

        for (iter = lo; iter < hi; iter++) {
            uint32_t copy = ge_clone(P, body_n, gvar_off, gvar_len, iter);
            if (copy == 0) break;
            /* Insert after prev in sibling chain */
            P->nodes[copy].next_sib = P->nodes[prev].next_sib;
            P->nodes[prev].next_sib = copy;
            prev = copy;
            unrolled++;
        }
    }

    /* Kill the original FOR node */
    ge_kill(P, for_n);

    return unrolled;
}

/* Walk AST looking for GENERATE > FOR patterns and unroll them */

static int
ge_unroll(tk_parse_t *P, uint32_t node)
{
    uint32_t c;
    int count = 0;

    if (node == 0 || KA_CHK(node, P->n_node)) return 0;
    if (P->nodes[node].type == TK_AST_COUNT) return 0;

    /* GENERATE node containing a FOR child */
    if (P->nodes[node].type == TK_AST_GENERATE) {
        c = P->nodes[node].first_child;
        KA_GUARD(g, 100);
        while (c && g--) {
            if (P->nodes[c].type == TK_AST_FOR) {
                count += ge_unfor(P, c, node);
            }
            c = P->nodes[c].next_sib;
        }
    }

    /* Recurse into children */
    c = P->nodes[node].first_child;
    KA_GUARD(g2, 100000);
    while (c && g2--) {
        if (P->nodes[c].type != TK_AST_COUNT)
            count += ge_unroll(P, c);
        c = P->nodes[c].next_sib;
    }

    return count;
}

/* ---- Count dead nodes ---- */

static int
ge_count(const tk_parse_t *P)
{
    uint32_t i;
    int dead = 0;
    for (i = 1; i < P->n_node; i++) {
        if (P->nodes[i].type == TK_AST_COUNT)
            dead++;
    }
    return dead;
}

/* ---- Public API ---- */

int
ge_expand(tk_parse_t *P)
{
    int pruned;
    int dead;

    if (!P) return -1;

    {
        int unrolled = ge_unroll(P, 1);
        if (unrolled > 0)
            printf("takahe: gexp: %d generate-for iterations unrolled\n",
                   unrolled);
    }

    pruned = ge_prune(P, 1);  /* node 1 = ROOT */
    dead = ge_count(P);

    printf("takahe: gexp: %d branches pruned, %d nodes dead\n",
           pruned, dead);

    return pruned;
}
