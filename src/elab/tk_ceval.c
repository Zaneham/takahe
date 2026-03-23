/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_ceval.c -- Constant expression evaluator for Takahe
 *
 * Evaluates compile-time constant expressions in the AST.
 * Used by elaboration to resolve parameter values, generate
 * conditions, and array dimensions before synthesis.
 *
 * Walks the AST bottom-up, evaluating nodes that have all
 * constant children. Leaves non-constant nodes untouched.
 * Like a tax inspector: only touches the numbers, never
 * the narrative.
 *
 * Supports: integer arithmetic, bitwise ops, shifts,
 * comparisons, logical ops, ternary, $clog2.
 * Does NOT support: reals, strings, or anything that
 * requires runtime information.
 *
 * explicit stack, no floats in the evaluator.
 */

#include "takahe.h"

/* ---- Result helpers (ce_val_t defined in takahe.h) ---- */

#define CE_OK(v, w)   ((ce_val_t){ (v), (w), 1, 0 })
#define CE_SGN(v, w)  ((ce_val_t){ (v), (w), 1, 1 })
#define CE_FAIL       ((ce_val_t){ 0, 0, 0, 0 })

/* ---- Parse integer literal text ----
 * Handles: plain decimal, based (4'b1010, 8'hFF),
 * unbased unsized ('0, '1), underscores. */

static ce_val_t
ce_ilit(const char *s, uint16_t len)
{
    int64_t val = 0;
    uint32_t width = 32;  /* default */
    int base = 10;
    int i = 0;
    int sgn = 0;

    if (len == 0 || !s) return CE_FAIL;

    /* Unbased unsized: '0, '1 */
    if (s[0] == '\'') {
        if (len >= 2) {
            if (s[1] == '0') return CE_OK(0, 1);
            if (s[1] == '1') return CE_OK(1, 1);
        }
        return CE_FAIL;
    }

    /* Scan for tick to separate size from value */
    int tick = -1;
    KA_GUARD(g, 64);
    for (i = 0; i < len && g--; i++) {
        if (s[i] == '\'') { tick = i; break; }
    }

    if (tick >= 0) {
        /* Parse size prefix */
        width = 0;
        for (i = 0; i < tick; i++) {
            if (s[i] >= '0' && s[i] <= '9')
                width = width * 10 + (uint32_t)(s[i] - '0');
        }
        if (width == 0) width = 32;

        /* Parse base */
        i = tick + 1;
        if (i < len && (s[i] == 's' || s[i] == 'S')) {
            sgn = 1;
            i++;
        }
        if (i < len) {
            switch (s[i]) {
            case 'b': case 'B': base = 2; i++; break;
            case 'o': case 'O': base = 8; i++; break;
            case 'd': case 'D': base = 10; i++; break;
            case 'h': case 'H': base = 16; i++; break;
            default: break;
            }
        }

        /* Skip whitespace after base */
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;

        /* Parse digits */
        val = 0;
        KA_GUARD(g2, 128);
        for (; i < len && g2--; i++) {
            char c = s[i];
            if (c == '_') continue;
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') digit = 10 + c - 'A';
            else if (c == 'x' || c == 'X' || c == 'z' || c == 'Z' ||
                     c == '?')
                return CE_FAIL;  /* x/z not constant */
            if (digit < 0 || digit >= base) continue;
            val = val * base + digit;
        }
    } else {
        /* Plain decimal */
        val = 0;
        KA_GUARD(g3, 64);
        for (i = 0; i < len && g3--; i++) {
            if (s[i] == '_') continue;
            if (s[i] >= '0' && s[i] <= '9')
                val = val * 10 + (s[i] - '0');
        }
        width = 32;
    }

    /* Mask to width */
    if (width > 0 && width < 64) {
        uint64_t mask = (1ULL << width) - 1;
        val = (int64_t)((uint64_t)val & mask);
    }

    return sgn ? CE_SGN(val, width) : CE_OK(val, width);
}

/* ---- $clog2 ---- */

static int64_t
ce_clog2(int64_t v)
{
    int64_t r = 0;
    if (v <= 0) return 0;
    v--;
    while (v > 0) { v >>= 1; r++; }
    return r;
}

/* ---- Evaluate one AST node ----
 * Returns CE_FAIL if the node can't be constant-evaluated.
 * Non-recursive: only evaluates leaf nodes and nodes whose
 * children have already been evaluated (bottom-up pass). */

static ce_val_t
ce_node(const tk_parse_t *P, uint32_t nidx,
        const ce_val_t *vals, uint32_t nvals)
{
    const tk_node_t *n;
    ce_val_t l, r;
    uint32_t c1, c2;

    if (nidx == 0 || KA_CHK(nidx, P->n_node)) return CE_FAIL;
    n = &P->nodes[nidx];

    switch (n->type) {
    case TK_AST_INT_LIT:
        return ce_ilit(P->lex->strs + n->d.text.off, n->d.text.len);

    case TK_AST_IDENT:
        /* Identifier: check if it's a known param value.
         * vals[] is indexed by node index. If the ident
         * was resolved by parameter substitution, its
         * value will be in vals[nidx]. */
        if (nidx < nvals && vals[nidx].valid)
            return vals[nidx];
        return CE_FAIL;

    case TK_AST_BINARY_OP:
        c1 = n->first_child;
        if (c1 == 0) return CE_FAIL;
        c2 = P->nodes[c1].next_sib;
        if (c2 == 0) return CE_FAIL;

        if (c1 >= nvals || c2 >= nvals) return CE_FAIL;
        l = vals[c1];
        r = vals[c2];
        if (!l.valid || !r.valid) return CE_FAIL;

        {
            const char *op = P->lex->strs +
                P->lex->ops[n->op].chars_off;

            if (strcmp(op, "+") == 0)  return CE_OK(l.val + r.val, l.width);
            if (strcmp(op, "-") == 0)  return CE_OK(l.val - r.val, l.width);
            if (strcmp(op, "*") == 0)  return CE_OK(l.val * r.val, l.width);
            if (strcmp(op, "/") == 0) {
                if (r.val == 0) return CE_FAIL;
                return CE_OK(l.val / r.val, l.width);
            }
            if (strcmp(op, "%") == 0) {
                if (r.val == 0) return CE_FAIL;
                return CE_OK(l.val % r.val, l.width);
            }
            if (strcmp(op, "**") == 0) {
                int64_t res = 1;
                int64_t base = l.val;
                int64_t exp = r.val;
                KA_GUARD(gp, 64);
                while (exp > 0 && gp--) {
                    if (exp & 1) res *= base;
                    base *= base;
                    exp >>= 1;
                }
                return CE_OK(res, l.width);
            }

            if (strcmp(op, "&") == 0)   return CE_OK(l.val & r.val, l.width);
            if (strcmp(op, "|") == 0)   return CE_OK(l.val | r.val, l.width);
            if (strcmp(op, "^") == 0)   return CE_OK(l.val ^ r.val, l.width);
            if (strcmp(op, "~^") == 0 || strcmp(op, "^~") == 0)
                return CE_OK(~(l.val ^ r.val), l.width);

            if (strcmp(op, "<<") == 0)  return CE_OK(l.val << r.val, l.width);
            if (strcmp(op, ">>") == 0)  return CE_OK((int64_t)((uint64_t)l.val >> r.val), l.width);
            if (strcmp(op, "<<<") == 0) return CE_OK(l.val << r.val, l.width);
            if (strcmp(op, ">>>") == 0) return CE_OK(l.val >> r.val, l.width);

            if (strcmp(op, "==") == 0)  return CE_OK(l.val == r.val, 1);
            if (strcmp(op, "!=") == 0)  return CE_OK(l.val != r.val, 1);
            if (strcmp(op, "===") == 0) return CE_OK(l.val == r.val, 1);
            if (strcmp(op, "!==") == 0) return CE_OK(l.val != r.val, 1);
            if (strcmp(op, "<") == 0)   return CE_OK(l.val < r.val, 1);
            if (strcmp(op, ">") == 0)   return CE_OK(l.val > r.val, 1);
            if (strcmp(op, "<=") == 0)  return CE_OK(l.val <= r.val, 1);
            if (strcmp(op, ">=") == 0)  return CE_OK(l.val >= r.val, 1);

            if (strcmp(op, "&&") == 0)  return CE_OK(l.val && r.val, 1);
            if (strcmp(op, "||") == 0)  return CE_OK(l.val || r.val, 1);
        }
        return CE_FAIL;

    case TK_AST_UNARY_OP:
        c1 = n->first_child;
        if (c1 == 0 || c1 >= nvals) return CE_FAIL;
        l = vals[c1];
        if (!l.valid) return CE_FAIL;

        {
            const char *op = P->lex->strs +
                P->lex->ops[n->op].chars_off;

            if (strcmp(op, "-") == 0)  return CE_OK(-l.val, l.width);
            if (strcmp(op, "+") == 0)  return l;
            if (strcmp(op, "!") == 0)  return CE_OK(!l.val, 1);
            if (strcmp(op, "~") == 0)  return CE_OK(~l.val, l.width);

            /* Reduction operators */
            if (strcmp(op, "&") == 0) {
                uint64_t m = l.width < 64 ? (1ULL << l.width) - 1 : ~0ULL;
                return CE_OK(((uint64_t)l.val & m) == m, 1);
            }
            if (strcmp(op, "|") == 0) {
                return CE_OK(l.val != 0, 1);
            }
            if (strcmp(op, "^") == 0) {
                /* XOR reduction: count set bits, return parity */
                uint64_t v = (uint64_t)l.val;
                int cnt = 0;
                KA_GUARD(gb, 64);
                while (v && gb--) { cnt += (int)(v & 1); v >>= 1; }
                return CE_OK(cnt & 1, 1);
            }
        }
        return CE_FAIL;

    case TK_AST_TERNARY:
        c1 = n->first_child;
        if (c1 == 0 || c1 >= nvals) return CE_FAIL;
        l = vals[c1];
        if (!l.valid) return CE_FAIL;

        c2 = P->nodes[c1].next_sib;
        if (c2 == 0 || c2 >= nvals) return CE_FAIL;
        {
            uint32_t c3 = P->nodes[c2].next_sib;
            if (c3 == 0 || c3 >= nvals) return CE_FAIL;
            if (!vals[c2].valid || !vals[c3].valid) return CE_FAIL;
            return l.val ? vals[c2] : vals[c3];
        }

    case TK_AST_CALL:
        /* System functions: $clog2, $bits */
        if (n->d.text.len > 0) {
            const char *fn = P->lex->strs + n->d.text.off;
            c1 = n->first_child;
            if (c1 == 0 || c1 >= nvals) return CE_FAIL;
            l = vals[c1];
            if (!l.valid) return CE_FAIL;

            if (strcmp(fn, "$clog2") == 0)
                return CE_OK(ce_clog2(l.val), 32);
            if (strcmp(fn, "$bits") == 0)
                return CE_OK(l.val, 32);
            if (strcmp(fn, "$signed") == 0 ||
                strcmp(fn, "$unsigned") == 0)
                return CE_OK(l.val, l.width);
        }
        return CE_FAIL;

    case TK_AST_ROOT:      case TK_AST_MODULE:
    case TK_AST_PORT:      case TK_AST_PARAM:
    case TK_AST_LOCALPARAM:case TK_AST_TYPEDEF:
    case TK_AST_ENUM_DEF:  case TK_AST_STRUCT_DEF:
    case TK_AST_MEMBER:    case TK_AST_TYPE_SPEC:
    case TK_AST_NET_DECL:  case TK_AST_VAR_DECL:
    case TK_AST_ASSIGN:    case TK_AST_BLOCK_ASSIGN:
    case TK_AST_NONBLOCK:  case TK_AST_ALWAYS_COMB:
    case TK_AST_ALWAYS_FF: case TK_AST_ALWAYS_LATCH:
    case TK_AST_ALWAYS:    case TK_AST_SENS_LIST:
    case TK_AST_SENS_EDGE: case TK_AST_IF:
    case TK_AST_CASE:      case TK_AST_CASE_ITEM:
    case TK_AST_FOR:       case TK_AST_WHILE:
    case TK_AST_BEGIN_END: case TK_AST_GENERATE:
    case TK_AST_GENVAR:    case TK_AST_GEN_FOR:
    case TK_AST_GEN_IF:    case TK_AST_REAL_LIT:
    case TK_AST_STR_LIT:   case TK_AST_CONCAT:
    case TK_AST_REPLICATE: case TK_AST_INDEX:
    case TK_AST_RANGE:     case TK_AST_MEMBER_ACC:
    case TK_AST_CAST:      case TK_AST_INSTANCE:
    case TK_AST_CONN:      case TK_AST_COUNT:
    default:
        return CE_FAIL;
    }
}

/* ---- Bottom-up evaluation pass ----
 * Walk all nodes in index order (which is roughly bottom-up
 * since children are allocated before parents in recursive
 * descent). For each node, try to evaluate it from its
 * children's values. Repeat until no new evaluations occur
 * (fixpoint). Bounded by node count. */

int
ce_eval(const tk_parse_t *P, ce_val_t *vals, uint32_t nvals)
{
    uint32_t i;
    int changed;
    int total = 0;

    if (!P || !vals || nvals == 0) return 0;

    /* Don't zero vals — caller may have pre-seeded values
     * (e.g. parameter substitutions from elaboration).
     * We only fill in nodes that aren't already valid. */

    /* Fixpoint iteration — like power iteration in Moa,
     * except the eigenvalue is "did we learn anything new"
     * and k_eff is always 0 or 1. */
    KA_GUARD(g, 100);  /* max 100 iterations */
    do {
        changed = 0;
        for (i = 1; i < P->n_node && i < nvals; i++) {
            if (vals[i].valid) continue;  /* already done */
            ce_val_t v = ce_node(P, i, vals, nvals);
            if (v.valid) {
                vals[i] = v;
                changed = 1;
                total++;
            }
        }
    } while (changed && g--);

    return total;
}
