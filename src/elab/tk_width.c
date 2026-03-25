/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_width.c -- Width inference for Takahe
 *
 * Resolves bit widths of all nets and expressions in the AST.
 * After elaboration, parameterised ranges like [WIDTH-1:0] have
 * concrete constant values. This pass walks the AST and computes
 * the actual bit width of every signal, port, and expression.
 *
 * IEEE 1800-2017 Section 11.6: expression bit lengths.
 * The rules are 40 pages of "it depends" that would make a
 * tax lawyer weep. We implement the synthesisable subset.
 *
 * Key rules:
 *   - logic [7:0] x  -> width 8
 *   - integer        -> width 32
 *   - bit            -> width 1
 *   - a + b          -> max(width(a), width(b))
 *   - a == b         -> width 1 (comparison)
 *   - a ? b : c      -> max(width(b), width(c))
 *   - {a, b}         -> width(a) + width(b)
 *   - {N{a}}         -> N * width(a)
 *
 * than the AST nesting depth.
 */

#include "takahe.h"

/* wi_val_t defined in takahe.h */

/* ---- Default widths for type keywords ---- */

static uint32_t
wi_tdef(const tk_parse_t *P, uint32_t nidx)
{
    const tk_node_t *n;
    const char *tn;

    if (nidx == 0 || KA_CHK(nidx, P->n_node)) return 0;
    n = &P->nodes[nidx];
    if (n->type != TK_AST_TYPE_SPEC) return 0;
    if (n->d.text.len == 0) return 0;

    tn = P->lex->strs + n->d.text.off;

    /* Types with implicit widths */
    if (strcmp(tn, "integer") == 0) return 32;
    if (strcmp(tn, "int") == 0)     return 32;
    if (strcmp(tn, "shortint") == 0) return 16;
    if (strcmp(tn, "longint") == 0) return 64;
    if (strcmp(tn, "byte") == 0)    return 8;
    if (strcmp(tn, "bit") == 0)     return 1;
    if (strcmp(tn, "logic") == 0)   return 1;  /* without range */
    if (strcmp(tn, "reg") == 0)     return 1;  /* without range */
    if (strcmp(tn, "wire") == 0)    return 1;  /* without range */
    if (strcmp(tn, "real") == 0)    return 64;
    if (strcmp(tn, "shortreal") == 0) return 32;
    if (strcmp(tn, "time") == 0)    return 64;
    if (strcmp(tn, "realtime") == 0) return 64;

    /* Check user-defined type names (typedef enum/struct).
     * If someone typedef'd a 2-bit enum, the port that uses
     * it should be 2 bits wide. Obvious, but took us a while
     * to get here. */
    {
        uint32_t ti;
        uint16_t tlen = n->d.text.len;
        for (ti = 0; ti < P->n_tname; ti++) {
            if (P->tnames[ti].len == tlen &&
                memcmp(P->lex->strs + P->tnames[ti].off,
                       tn, tlen) == 0) {
                if (P->tnames[ti].width > 0)
                    return P->tnames[ti].width;
                break;
            }
        }
    }

    return 1; /* default: single bit */
}

/* ---- Compute width from a RANGE node [hi:lo] ---- */

static uint32_t
wi_range(const tk_parse_t *P, const ce_val_t *cv,
         uint32_t nvals, uint32_t nidx)
{
    const tk_node_t *n;
    uint32_t hi_n, lo_n;
    int64_t hi_v, lo_v;

    if (nidx == 0 || KA_CHK(nidx, P->n_node)) return 0;
    n = &P->nodes[nidx];
    if (n->type != TK_AST_RANGE) return 0;

    /* First child = high expr, second = low expr */
    hi_n = n->first_child;
    if (hi_n == 0) return 0;
    lo_n = P->nodes[hi_n].next_sib;
    if (lo_n == 0) return 0;

    /* Both must be constant-evaluated */
    if (hi_n >= nvals || lo_n >= nvals) return 0;
    if (!cv[hi_n].valid || !cv[lo_n].valid) return 0;

    hi_v = cv[hi_n].val;
    lo_v = cv[lo_n].val;

    /* Width = |hi - lo| + 1 */
    if (hi_v >= lo_v)
        return (uint32_t)(hi_v - lo_v + 1);
    else
        return (uint32_t)(lo_v - hi_v + 1);
}

/* ---- Resolve width of a TYPE_SPEC node ----
 * TYPE_SPEC may have a RANGE child: logic [7:0] -> 8.
 * If no range, use the default for the type keyword. */

static uint32_t
wi_type(const tk_parse_t *P, const ce_val_t *cv,
        uint32_t nvals, uint32_t nidx)
{
    const tk_node_t *n;
    uint32_t c;

    if (nidx == 0 || KA_CHK(nidx, P->n_node)) return 0;
    n = &P->nodes[nidx];
    if (n->type != TK_AST_TYPE_SPEC) return 0;

    /* Check for RANGE child */
    c = n->first_child;
    KA_GUARD(g, 10);
    while (c && g--) {
        if (P->nodes[c].type == TK_AST_RANGE) {
            uint32_t w = wi_range(P, cv, nvals, c);
            if (w > 0) return w;
        }
        c = P->nodes[c].next_sib;
    }

    /* No range: use type default */
    return wi_tdef(P, nidx);
}

/* ---- Walk AST and compute widths ----
 * Bottom-up: compute widths for declarations first (they
 * have explicit types), then propagate to expressions.
 * Returns number of widths resolved. */

int
wi_eval(const tk_parse_t *P, const ce_val_t *cv,
        uint32_t nvals, wi_val_t *wv, uint32_t nwv)
{
    uint32_t i, c;
    int total = 0;
    int changed;

    if (!P || !cv || !wv) return 0;

    memset(wv, 0, nwv * sizeof(wi_val_t));

    /* Pass 1: resolve widths from type specifiers and literals */
    for (i = 1; i < P->n_node && i < nwv; i++) {
        const tk_node_t *n = &P->nodes[i];

        switch (n->type) {
        case TK_AST_TYPE_SPEC:
            wv[i].width = wi_type(P, cv, nvals, i);
            if (wv[i].width > 0) {
                wv[i].resolved = 1;
                total++;
            }
            break;

        case TK_AST_INT_LIT:
            /* Literal width from constant evaluator */
            if (i < nvals && cv[i].valid) {
                wv[i].width = cv[i].width;
                if (wv[i].width == 0) wv[i].width = 32;
                wv[i].resolved = 1;
                total++;
            }
            break;

        case TK_AST_PORT:
        case TK_AST_NET_DECL:
        case TK_AST_VAR_DECL:
            /* Width comes from the TYPE_SPEC child */
            c = n->first_child;
            KA_GUARD(g1, 10);
            while (c && g1--) {
                if (P->nodes[c].type == TK_AST_TYPE_SPEC) {
                    wv[i].width = wi_type(P, cv, nvals, c);
                    if (wv[i].width > 0) {
                        wv[i].resolved = 1;
                        total++;
                    }
                    break;
                }
                c = P->nodes[c].next_sib;
            }
            break;

        case TK_AST_PARAM:
        case TK_AST_LOCALPARAM:
            /* Width from type child or value */
            c = n->first_child;
            KA_GUARD(g2, 10);
            while (c && g2--) {
                if (P->nodes[c].type == TK_AST_TYPE_SPEC) {
                    wv[i].width = wi_type(P, cv, nvals, c);
                    if (wv[i].width > 0) {
                        wv[i].resolved = 1;
                        total++;
                    }
                    break;
                }
                c = P->nodes[c].next_sib;
            }
            if (!wv[i].resolved && i < nvals && cv[i].valid) {
                wv[i].width = cv[i].width;
                if (wv[i].width == 0) wv[i].width = 32;
                wv[i].resolved = 1;
                total++;
            }
            break;

        case TK_AST_ROOT:       case TK_AST_MODULE:
        case TK_AST_TYPEDEF:    case TK_AST_ENUM_DEF:
        case TK_AST_STRUCT_DEF: case TK_AST_MEMBER:
        case TK_AST_ASSIGN:     case TK_AST_BLOCK_ASSIGN:
        case TK_AST_NONBLOCK:   case TK_AST_ALWAYS_COMB:
        case TK_AST_ALWAYS_FF:  case TK_AST_ALWAYS_LATCH:
        case TK_AST_ALWAYS:     case TK_AST_SENS_LIST:
        case TK_AST_SENS_EDGE:  case TK_AST_IF:
        case TK_AST_CASE:       case TK_AST_CASE_ITEM:
        case TK_AST_FOR:        case TK_AST_WHILE:
        case TK_AST_BEGIN_END:  case TK_AST_GENERATE:
        case TK_AST_GENVAR:     case TK_AST_GEN_FOR:
        case TK_AST_GEN_IF:     case TK_AST_REAL_LIT:
        case TK_AST_STR_LIT:    case TK_AST_BINARY_OP:
        case TK_AST_UNARY_OP:   case TK_AST_TERNARY:
        case TK_AST_CONCAT:     case TK_AST_REPLICATE:
        case TK_AST_INDEX:      case TK_AST_RANGE:
        case TK_AST_MEMBER_ACC: case TK_AST_CALL:
        case TK_AST_CAST:       case TK_AST_INSTANCE:
        case TK_AST_CONN:       case TK_AST_IDENT:
        case TK_AST_COUNT:
        default:
            break;
        }
    }

    /* Pass 2: propagate widths through expressions (fixpoint) */
    KA_GUARD(gfp, 50);
    do {
        changed = 0;
        for (i = 1; i < P->n_node && i < nwv; i++) {
            const tk_node_t *n = &P->nodes[i];
            uint32_t c1, c2, w;

            if (wv[i].resolved) continue;

            switch (n->type) {
            case TK_AST_BINARY_OP:
                c1 = n->first_child;
                if (c1 == 0) break;
                c2 = P->nodes[c1].next_sib;
                if (c2 == 0) break;
                if (c1 < nwv && c2 < nwv &&
                    wv[c1].resolved && wv[c2].resolved) {
                    /* Check if comparison operator */
                    const char *op = P->lex->strs +
                        P->lex->ops[n->op].chars_off;
                    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                        strcmp(op, "===") == 0 || strcmp(op, "!==") == 0 ||
                        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
                        strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
                        wv[i].width = 1;
                    } else {
                        /* Arithmetic/bitwise: max of operands */
                        w = wv[c1].width > wv[c2].width ?
                            wv[c1].width : wv[c2].width;
                        wv[i].width = w;
                    }
                    wv[i].resolved = 1;
                    changed = 1;
                    total++;
                }
                break;

            case TK_AST_UNARY_OP:
                c1 = n->first_child;
                if (c1 && c1 < nwv && wv[c1].resolved) {
                    const char *op = P->lex->strs +
                        P->lex->ops[n->op].chars_off;
                    /* Reduction and logical: 1-bit result */
                    if (strcmp(op, "!") == 0 ||
                        strcmp(op, "&") == 0 ||
                        strcmp(op, "|") == 0 ||
                        strcmp(op, "^") == 0) {
                        wv[i].width = 1;
                    } else {
                        wv[i].width = wv[c1].width;
                    }
                    wv[i].resolved = 1;
                    changed = 1;
                    total++;
                }
                break;

            case TK_AST_TERNARY:
                c1 = n->first_child;
                if (c1 == 0) break;
                c2 = P->nodes[c1].next_sib;
                if (c2 == 0) break;
                {
                    uint32_t c3 = P->nodes[c2].next_sib;
                    if (c3 == 0) break;
                    if (c2 < nwv && c3 < nwv &&
                        wv[c2].resolved && wv[c3].resolved) {
                        w = wv[c2].width > wv[c3].width ?
                            wv[c2].width : wv[c3].width;
                        wv[i].width = w;
                        wv[i].resolved = 1;
                        changed = 1;
                        total++;
                    }
                }
                break;

            case TK_AST_CONCAT:
                /* Sum of all children widths */
                {
                    uint32_t sum = 0;
                    int all_ok = 1;
                    c = n->first_child;
                    KA_GUARD(gc, 100);
                    while (c && gc--) {
                        if (c < nwv && wv[c].resolved)
                            sum += wv[c].width;
                        else
                            all_ok = 0;
                        c = P->nodes[c].next_sib;
                    }
                    if (all_ok && sum > 0) {
                        wv[i].width = sum;
                        wv[i].resolved = 1;
                        changed = 1;
                        total++;
                    }
                }
                break;

            case TK_AST_REPLICATE:
                /* N * width(inner) */
                c1 = n->first_child;  /* count */
                if (c1 == 0) break;
                c2 = P->nodes[c1].next_sib;  /* inner expr */
                if (c2 == 0) break;
                if (c1 < nvals && cv[c1].valid &&
                    c2 < nwv && wv[c2].resolved) {
                    wv[i].width = (uint32_t)cv[c1].val * wv[c2].width;
                    wv[i].resolved = 1;
                    changed = 1;
                    total++;
                }
                break;

            case TK_AST_INDEX:
                /* Part-select or bit-select.
                 * a[7:0] → INDEX(IDENT, RANGE) → range width
                 * a[5]   → INDEX(IDENT, INT_LIT) → 1 bit */
                c1 = n->first_child;
                if (c1) {
                    c2 = P->nodes[c1].next_sib;
                    if (c2 && P->nodes[c2].type == TK_AST_RANGE) {
                        w = wi_range(P, cv, nvals, c2);
                        if (w > 0) {
                            wv[i].width = w;
                            wv[i].resolved = 1;
                            changed = 1; total++;
                        }
                    } else {
                        /* Single index = bit select → 1 */
                        wv[i].width = 1;
                        wv[i].resolved = 1;
                        changed = 1; total++;
                    }
                }
                break;

            case TK_AST_RANGE:
                /* Part select: |hi-lo|+1 */
                w = wi_range(P, cv, nvals, i);
                if (w > 0) {
                    wv[i].width = w;
                    wv[i].resolved = 1;
                    changed = 1;
                    total++;
                }
                break;

            case TK_AST_CALL:
                /* $signed(expr), $unsigned(expr): same width as arg */
                c1 = n->first_child;
                if (c1 && c1 < nwv && wv[c1].resolved) {
                    wv[i].width = wv[c1].width;
                    wv[i].resolved = 1;
                    changed = 1;
                    total++;
                }
                break;

            case TK_AST_IDENT:
                /* Inherit from constant eval if available */
                if (i < nvals && cv[i].valid && cv[i].width > 0) {
                    wv[i].width = cv[i].width;
                    wv[i].resolved = 1;
                    changed = 1;
                    total++;
                }
                break;

            case TK_AST_ROOT:       case TK_AST_MODULE:
            case TK_AST_PORT:       case TK_AST_PARAM:
            case TK_AST_LOCALPARAM: case TK_AST_TYPEDEF:
            case TK_AST_ENUM_DEF:   case TK_AST_STRUCT_DEF:
            case TK_AST_MEMBER:     case TK_AST_TYPE_SPEC:
            case TK_AST_NET_DECL:   case TK_AST_VAR_DECL:
            case TK_AST_ASSIGN:     case TK_AST_BLOCK_ASSIGN:
            case TK_AST_NONBLOCK:   case TK_AST_ALWAYS_COMB:
            case TK_AST_ALWAYS_FF:  case TK_AST_ALWAYS_LATCH:
            case TK_AST_ALWAYS:     case TK_AST_SENS_LIST:
            case TK_AST_SENS_EDGE:  case TK_AST_IF:
            case TK_AST_CASE:       case TK_AST_CASE_ITEM:
            case TK_AST_FOR:        case TK_AST_WHILE:
            case TK_AST_BEGIN_END:  case TK_AST_GENERATE:
            case TK_AST_GENVAR:     case TK_AST_GEN_FOR:
            case TK_AST_GEN_IF:     case TK_AST_INT_LIT:
            case TK_AST_REAL_LIT:   case TK_AST_STR_LIT:
            case TK_AST_MEMBER_ACC:
            case TK_AST_CAST:       case TK_AST_INSTANCE:
            case TK_AST_CONN:       case TK_AST_COUNT:
            default:
                break;
            }
        }
    } while (changed && gfp--);

    return total;
}
