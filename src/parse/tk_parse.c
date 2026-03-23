/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_parse.c -- SystemVerilog parser for Takahe
 *
 * Recursive descent parser for the synthesisable subset of
 * IEEE 1800-2017. Produces a flat AST in pre-allocated pools.
 *
 * Parsing strategy: we only parse what can be synthesised.
 * class, program, covergroup, fork/join, and the rest of the
 * verification world are politely ignored. If you want those,
 * Synopsys is right over there. Bring your chequebook.
 *
 * Same helper pattern as BarraCUDA (parser.c) and Karearea
 * (f7_parse.c): peek/advance/expect/match on token stream,
 * pk_alloc for AST nodes, pk_achld for tree construction.
 *
 *
 */

#include "takahe.h"

/* ---- Keyword ID Lookup ----
 * Find a keyword by name in the sorted table. Returns its
 * table index, or TK_KW_NONE if absent. Called once per
 * keyword at parser init, never in the hot path. */

static uint16_t
pk_kwfn(const tk_lex_t *L, const char *name)
{
    uint32_t i;
    uint16_t len = (uint16_t)strlen(name);
    for (i = 0; i < L->n_kwd; i++) {
        if (L->kwds[i].name_len == len &&
            memcmp(L->strs + L->kwds[i].name_off, name, len) == 0)
            return (uint16_t)i;
    }
    return TK_KW_NONE;
}

/* ---- Token Stream Helpers ---- */

static const tk_token_t *
cur(const tk_parse_t *P)
{
    if (P->pos >= P->n_tok) return &P->tokens[P->n_tok - 1]; /* EOF */
    return &P->tokens[P->pos];
}

static tk_toktype_t
pk_ctyp(const tk_parse_t *P)
{
    return cur(P)->type;
}

static uint16_t
pk_csub(const tk_parse_t *P)
{
    return cur(P)->sub;
}

static void
advance(tk_parse_t *P)
{
    if (P->pos < P->n_tok) P->pos++;
}

/* Check current token against a pre-computed keyword ID.
 * Integer compare, not strcmp. The parser's inner loop
 * thanks us for this small mercy. */
static int
pk_iskw(const tk_parse_t *P, uint16_t kwid)
{
    if (kwid == TK_KW_NONE) return 0;
    const tk_token_t *t = cur(P);
    return t->type == TK_TOK_KWD && t->sub == kwid;
}



/* Check if current token is a specific operator (by chars) */
static int
is_op(const tk_parse_t *P, const char *chars)
{
    const tk_token_t *t = cur(P);
    if (t->type != TK_TOK_OP) return 0;
    const char *oc = P->lex->strs + P->lex->ops[t->sub].chars_off;
    return strcmp(oc, chars) == 0;
}

static int
pk_mkw(tk_parse_t *P, uint16_t kwid)
{
    if (pk_iskw(P, kwid)) { advance(P); return 1; }
    return 0;
}



static int
pk_mop(tk_parse_t *P, const char *chars)
{
    if (is_op(P, chars)) { advance(P); return 1; }
    return 0;
}

static void
pk_ekw(tk_parse_t *P, uint16_t kwid)
{
    if (!pk_mkw(P, kwid)) {
        const tk_token_t *t = cur(P);
        if (P->n_err < TK_MAX_ERRORS) {
            const char *expected = (kwid != TK_KW_NONE)
                ? P->lex->strs + P->lex->kwds[kwid].name_off
                : "?";
            tk_err_t *e = &P->errors[P->n_err++];
            e->line = t->line;
            e->col  = t->col;
            snprintf(e->msg, sizeof(e->msg),
                     "expected '%s', got '%.*s'",
                     expected, t->len > 30 ? 30 : t->len,
                     P->lex->strs + t->off);
        }
    }
}



static void
pk_eop(tk_parse_t *P, const char *chars)
{
    if (!pk_mop(P, chars)) {
        const tk_token_t *t = cur(P);
        if (P->n_err < TK_MAX_ERRORS) {
            tk_err_t *e = &P->errors[P->n_err++];
            e->line = t->line;
            e->col  = t->col;
            snprintf(e->msg, sizeof(e->msg),
                     "expected '%s', got '%.*s'",
                     chars, t->len > 30 ? 30 : t->len,
                     P->lex->strs + t->off);
        }
    }
}

/* ---- AST Node Management ---- */

static uint32_t
pk_alloc(tk_parse_t *P, tk_ast_type_t type)
{
    uint32_t idx;

    if (P->n_node >= P->max_node) return 0;  /* sentinel */

    idx = P->n_node++;
    memset(&P->nodes[idx], 0, sizeof(tk_node_t));
    P->nodes[idx].type = type;
    P->nodes[idx].line = cur(P)->line;
    P->nodes[idx].col  = cur(P)->col;

    return idx;
}

/* O(1) child insertion via last_child pointer.
 * The old code walked the sibling chain every time,
 * which is fine until someone writes a module with
 * 10,000 ports and wonders why parsing takes a nap. */
static void
pk_achld(tk_parse_t *P, uint32_t parent, uint32_t child)
{
    if (parent == 0 || child == 0) return;
    if (KA_CHK(parent, P->n_node)) return;
    if (KA_CHK(child, P->n_node)) return;

    /* Clear child's sibling link -- it's the new tail */
    P->nodes[child].next_sib = 0;

    if (P->nodes[parent].first_child == 0) {
        P->nodes[parent].first_child = child;
    } else {
        P->nodes[P->nodes[parent].last_child].next_sib = child;
    }
    P->nodes[parent].last_child = child;
}

/* ---- Error Recovery ----
 * When the parser trips over something unexpected, skip
 * tokens until we hit a synchronisation point. Like a
 * pilot regaining altitude after turbulence: find a known
 * landmark and resume navigation from there. */
static void
pk_sync(tk_parse_t *P)
{
    KA_GUARD(g, 10000);
    while (pk_ctyp(P) != TK_TOK_EOF && g--) {
        if (is_op(P, ";")) { advance(P); return; }
        if (pk_iskw(P, P->kw.endmodule)) return;
        if (pk_iskw(P, P->kw.end))       return;
        if (pk_iskw(P, P->kw.endcase))   return;
        if (pk_iskw(P, P->kw.endgenerate)) return;
        if (pk_iskw(P, P->kw.endtask))   return;
        if (pk_iskw(P, P->kw.endfunction)) return;
        advance(P);
    }
}

/* ---- Forward Declarations ---- */

static uint32_t pk_expr(tk_parse_t *P);
static uint32_t pk_stmt(tk_parse_t *P);
static void     pk_stms(tk_parse_t *P, uint32_t parent);
static void     pk_mbdy(tk_parse_t *P, uint32_t mod);

/* ---- Expression Parser ----
 * Operator precedence climbing. Same technique as BarraCUDA.
 * Precedence table from IEEE 1800-2017 Table 11-2. */

static int
op_prec(const tk_parse_t *P)
{
    if (pk_ctyp(P) != TK_TOK_OP) return -1;

    const char *c = P->lex->strs + P->lex->ops[pk_csub(P)].chars_off;

    if (strcmp(c, "||") == 0) return 2;
    if (strcmp(c, "&&") == 0) return 3;
    if (strcmp(c, "|") == 0)  return 4;
    if (strcmp(c, "^") == 0 || strcmp(c, "~^") == 0 ||
        strcmp(c, "^~") == 0) return 5;
    if (strcmp(c, "&") == 0)  return 6;
    if (strcmp(c, "==") == 0 || strcmp(c, "!=") == 0 ||
        strcmp(c, "===") == 0 || strcmp(c, "!==") == 0 ||
        strcmp(c, "==?") == 0 || strcmp(c, "!=?") == 0) return 7;
    if (strcmp(c, "<") == 0 || strcmp(c, ">") == 0 ||
        strcmp(c, ">=") == 0) return 8;
    if (strcmp(c, "<=") == 0) return P->no_le ? -1 : 8;
    if (strcmp(c, "<<") == 0 || strcmp(c, ">>") == 0 ||
        strcmp(c, "<<<") == 0 || strcmp(c, ">>>") == 0) return 9;
    if (strcmp(c, "+") == 0 || strcmp(c, "-") == 0) return 10;
    if (strcmp(c, "*") == 0 || strcmp(c, "/") == 0 ||
        strcmp(c, "%") == 0) return 11;
    if (strcmp(c, "**") == 0) return 12;

    return -1;
}

/* Primary expression */
static uint32_t
pk_prim(tk_parse_t *P)
{
    uint32_t n;

    /* Unary operators */
    if (pk_ctyp(P) == TK_TOK_OP) {
        const char *c = P->lex->strs + P->lex->ops[pk_csub(P)].chars_off;
        if (strcmp(c, "!") == 0 || strcmp(c, "~") == 0 ||
            strcmp(c, "-") == 0 || strcmp(c, "+") == 0 ||
            strcmp(c, "&") == 0 || strcmp(c, "|") == 0 ||
            strcmp(c, "^") == 0) {
            n = pk_alloc(P, TK_AST_UNARY_OP);
            P->nodes[n].op = pk_csub(P);
            advance(P);
            uint32_t operand = pk_prim(P);
            pk_achld(P, n, operand);
            return n;
        }
    }

    /* Parenthesised expression — re-enable <= as comparison
     * inside parens. if (a <= b) needs <= as binary op,
     * even when the outer context has no_le set. */
    if (is_op(P, "(")) {
        uint8_t saved = P->no_le;
        P->no_le = 0;
        advance(P);
        n = pk_expr(P);
        pk_eop(P, ")");
        P->no_le = saved;
        return n;
    }

    /* Concatenation {a, b, c} */
    if (is_op(P, "{")) {
        advance(P);
        n = pk_alloc(P, TK_AST_CONCAT);

        /* Check for replication: {N{expr}} */
        uint32_t first = pk_expr(P);
        if (is_op(P, "{")) {
            /* It's a replication */
            P->nodes[n].type = TK_AST_REPLICATE;
            pk_achld(P, n, first);  /* count */
            advance(P); /* eat inner { */
            uint32_t inner = pk_expr(P);
            pk_achld(P, n, inner);
            pk_eop(P, "}"); /* inner } */
        } else {
            pk_achld(P, n, first);
            KA_GUARD(g, 1000);
            while (is_op(P, ",") && g--) {
                advance(P);
                uint32_t elem = pk_expr(P);
                pk_achld(P, n, elem);
            }
        }
        pk_eop(P, "}");
        return n;
    }

    /* Integer literal */
    if (pk_ctyp(P) == TK_TOK_INT_LIT) {
        n = pk_alloc(P, TK_AST_INT_LIT);
        P->nodes[n].d.text.off = cur(P)->off;
        P->nodes[n].d.text.len = cur(P)->len;
        advance(P);
        return n;
    }

    /* Real literal */
    if (pk_ctyp(P) == TK_TOK_REAL_LIT) {
        n = pk_alloc(P, TK_AST_REAL_LIT);
        P->nodes[n].d.text.off = cur(P)->off;
        P->nodes[n].d.text.len = cur(P)->len;
        advance(P);
        return n;
    }

    /* String literal */
    if (pk_ctyp(P) == TK_TOK_STR_LIT) {
        n = pk_alloc(P, TK_AST_STR_LIT);
        P->nodes[n].d.text.off = cur(P)->off;
        P->nodes[n].d.text.len = cur(P)->len;
        advance(P);
        return n;
    }

    /* System task/function call ($clog2, $bits, etc.) */
    if (pk_ctyp(P) == TK_TOK_SYSTASK) {
        n = pk_alloc(P, TK_AST_CALL);
        P->nodes[n].d.text.off = cur(P)->off;
        P->nodes[n].d.text.len = cur(P)->len;
        advance(P);
        if (is_op(P, "(")) {
            advance(P);
            KA_GUARD(g, 100);
            while (!is_op(P, ")") && pk_ctyp(P) != TK_TOK_EOF && g--) {
                uint32_t arg = pk_expr(P);
                pk_achld(P, n, arg);
                if (!pk_mop(P, ",")) break;
            }
            pk_eop(P, ")");
        }
        return n;
    }

    /* Identifier (possibly with bit select, part select, member access) */
    if (pk_ctyp(P) == TK_TOK_IDENT || pk_ctyp(P) == TK_TOK_KWD) {
        n = pk_alloc(P, TK_AST_IDENT);
        P->nodes[n].d.text.off = cur(P)->off;
        P->nodes[n].d.text.len = cur(P)->len;
        advance(P);

        /* Postfix: bit select [expr], part select [hi:lo], member .name
         *
         * Build child chains directly. The old code called pk_achld
         * then overwrote first_child/next_sib, creating cycles that
         * turned a 49-line testcase into 20,000 lines of output.
         * Like wiring a feedback loop in your netlist — except
         * the netlist was the parser's own AST. */
        KA_GUARD(g, 100);
        while (g--) {
            if (is_op(P, "[")) {
                advance(P);
                uint32_t idx = pk_alloc(P, TK_AST_INDEX);
                uint32_t sub = pk_expr(P);

                if (sub != 0 &&
                    P->nodes[sub].type == TK_AST_BINARY_OP &&
                    P->nodes[sub].first_child != 0 &&
                    P->nodes[P->nodes[sub].first_child].next_sib == 0) {
                    /* Indexed part-select: base[offset +: width]
                     * pk_expr consumed +/- and : as a binary op
                     * but the RHS failed. The LHS of the binop
                     * is the real offset. The : was consumed by
                     * pk_prim's error path. Parse width next.
                     * Undo the false error from pk_prim. */
                    uint32_t off_n = P->nodes[sub].first_child;
                    uint16_t dir_op = P->nodes[sub].op;
                    P->nodes[sub].type = TK_AST_COUNT; /* dead */
                    if (P->n_err > 0) P->n_err--;  /* undo ':' error */

                    uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                    P->nodes[rng].op = dir_op;
                    uint32_t wid = pk_expr(P);

                    P->nodes[off_n].next_sib = wid;
                    P->nodes[wid].next_sib = 0;
                    P->nodes[rng].first_child = off_n;
                    P->nodes[rng].last_child = wid;

                    P->nodes[n].next_sib = rng;
                    P->nodes[rng].next_sib = 0;
                    P->nodes[idx].first_child = n;
                    P->nodes[idx].last_child = rng;
                } else if (is_op(P, ":")) {
                    /* Part select: base[hi:lo] → INDEX(base, RANGE(hi, lo))
                     * Keep idx as INDEX. Create inner RANGE for bounds. */
                    advance(P);
                    uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                    uint32_t lo = pk_expr(P);

                    /* RANGE children: hi, lo */
                    P->nodes[sub].next_sib = lo;
                    P->nodes[lo].next_sib = 0;
                    P->nodes[rng].first_child = sub;
                    P->nodes[rng].last_child = lo;

                    /* INDEX children: base, RANGE */
                    P->nodes[n].next_sib = rng;
                    P->nodes[rng].next_sib = 0;
                    P->nodes[idx].first_child = n;
                    P->nodes[idx].last_child = rng;
                } else {
                    /* Bit select: base[subscript] */
                    P->nodes[n].next_sib = sub;
                    P->nodes[sub].next_sib = 0;
                    P->nodes[idx].first_child = n;
                    P->nodes[idx].last_child = sub;
                }
                pk_eop(P, "]");
                n = idx;
            }
            else if (is_op(P, ".")) {
                /* Member access: base.field */
                advance(P);
                uint32_t macc = pk_alloc(P, TK_AST_MEMBER_ACC);
                uint32_t field = pk_alloc(P, TK_AST_IDENT);
                P->nodes[field].d.text.off = cur(P)->off;
                P->nodes[field].d.text.len = cur(P)->len;
                advance(P);

                P->nodes[n].next_sib = field;
                P->nodes[field].next_sib = 0;
                P->nodes[macc].first_child = n;
                P->nodes[macc].last_child = field;
                n = macc;
            }
            else {
                break;
            }
        }

        return n;
    }

    /* Fallthrough: unexpected token */
    if (P->n_err < TK_MAX_ERRORS) {
        const tk_token_t *t = cur(P);
        tk_err_t *e = &P->errors[P->n_err++];
        e->line = t->line;
        e->col  = t->col;
        snprintf(e->msg, sizeof(e->msg),
                 "unexpected token in expression: '%.*s'",
                 t->len > 30 ? 30 : t->len,
                 P->lex->strs + t->off);
    }
    advance(P);  /* skip to avoid infinite loop */
    return 0;
}

/* Expression with precedence climbing */
static uint32_t
pk_prec(tk_parse_t *P, int min_prec)
{
    uint32_t left = pk_prim(P);

    KA_GUARD(g, 1000);
    while (g--) {
        int prec = op_prec(P);
        if (prec < min_prec) break;

        uint32_t binop = pk_alloc(P, TK_AST_BINARY_OP);
        P->nodes[binop].op = pk_csub(P);
        advance(P);

        uint32_t right = pk_prec(P, prec + 1);

        P->nodes[binop].first_child = left;
        P->nodes[left].next_sib = right;
        left = binop;
    }

    /* Ternary: a ? b : c */
    if (is_op(P, "?")) {
        advance(P);
        uint32_t tern = pk_alloc(P, TK_AST_TERNARY);
        uint32_t then_e = pk_expr(P);
        pk_eop(P, ":");
        uint32_t else_e = pk_expr(P);
        P->nodes[tern].first_child = left;
        P->nodes[left].next_sib = then_e;
        P->nodes[then_e].next_sib = else_e;
        left = tern;
    }

    return left;
}

static uint32_t
pk_expr(tk_parse_t *P)
{
    return pk_prec(P, 1);
}

/* ---- Type Specifier ---- */

static uint32_t
pk_type(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_TYPE_SPEC);
    P->nodes[n].d.text.off = cur(P)->off;
    P->nodes[n].d.text.len = cur(P)->len;
    advance(P);

    /* Optional signedness */
    if (pk_iskw(P, P->kw.signed_kw) || pk_iskw(P, P->kw.unsigned_kw)) {
        advance(P);
    }

    /* Optional packed range [N-1:0] */
    if (is_op(P, "[")) {
        advance(P);
        uint32_t rng = pk_alloc(P, TK_AST_RANGE);
        uint32_t hi = pk_expr(P);
        pk_achld(P, rng, hi);
        pk_eop(P, ":");
        uint32_t lo = pk_expr(P);
        pk_achld(P, rng, lo);
        pk_eop(P, "]");
        pk_achld(P, n, rng);
    }

    return n;
}

/* ---- Type Name Registry ---- */

static void
pk_rtm(tk_parse_t *P, uint32_t off, uint16_t len)
{
    if (P->n_tname >= TK_MAX_TNAMES || len == 0) return;
    P->tnames[P->n_tname].off = off;
    P->tnames[P->n_tname].len = len;
    P->n_tname++;
}

static int
pk_istm(const tk_parse_t *P, uint32_t off, uint16_t len)
{
    uint32_t i;
    for (i = 0; i < P->n_tname; i++) {
        if (P->tnames[i].len == len &&
            memcmp(P->lex->strs + P->tnames[i].off,
                   P->lex->strs + off, len) == 0)
            return 1;
    }
    return 0;
}

/* Is current token a type keyword or registered type name?
 * Checked via cached IDs -- 13 integer compares instead
 * of 13 string comparisons, plus a linear scan of the
 * type name registry for user-defined types. The gate-level
 * equivalent of replacing an OR chain with a lookup table
 * plus a small CAM. */
static int
pk_isty(const tk_parse_t *P)
{
    if (pk_ctyp(P) == TK_TOK_KWD) {
        uint16_t s = pk_csub(P);
        if (s == P->kw.logic    || s == P->kw.wire     ||
            s == P->kw.reg      || s == P->kw.bit      ||
            s == P->kw.integer  || s == P->kw.kw_int   ||
            s == P->kw.byte_kw  || s == P->kw.shortint ||
            s == P->kw.longint  || s == P->kw.real     ||
            s == P->kw.shortreal|| s == P->kw.realtime ||
            s == P->kw.time_kw)
            return 1;
    }

    /* Check user-defined type names (from typedef/enum/struct) */
    if (pk_ctyp(P) == TK_TOK_IDENT) {
        const tk_token_t *t = cur(P);
        return pk_istm(P, t->off, t->len);
    }

    return 0;
}

/* ---- Statement Parser ---- */

static uint32_t
pk_blk(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_BEGIN_END);
    pk_ekw(P, P->kw.begin);

    /* Optional block label: begin : label_name */
    if (is_op(P, ":")) {
        advance(P);
        if (pk_ctyp(P) == TK_TOK_IDENT) {
            P->nodes[n].d.text.off = cur(P)->off;
            P->nodes[n].d.text.len = cur(P)->len;
            advance(P);
        }
    }

    pk_stms(P, n);
    pk_ekw(P, P->kw.end);

    /* Optional end label */
    if (is_op(P, ":")) {
        advance(P);
        if (pk_ctyp(P) == TK_TOK_IDENT) advance(P);
    }

    return n;
}

/* Forward declaration for mutual recursion */
static uint32_t pk_gif(tk_parse_t *P);

/* ---- Generate-body block: begin...end containing module items ----
 * Inside a generate if/else, a begin...end block holds wires,
 * always blocks, assigns — module-level items, not statements.
 * pk_blk only parses statements. This variant parses module items
 * using pk_mbdy with stop_end flag. */

static void pk_mbdy2(tk_parse_t *P, uint32_t mod, int stop_end);

static uint32_t
pk_gblk(tk_parse_t *P)
{
    if (pk_iskw(P, P->kw.begin)) {
        uint32_t n = pk_alloc(P, TK_AST_BEGIN_END);
        pk_ekw(P, P->kw.begin);

        /* Optional block label */
        if (is_op(P, ":")) {
            advance(P);
            if (pk_ctyp(P) == TK_TOK_IDENT) {
                P->nodes[n].d.text.off = cur(P)->off;
                P->nodes[n].d.text.len = cur(P)->len;
                advance(P);
            }
        }

        pk_mbdy2(P, n, 1);
        pk_ekw(P, P->kw.end);

        /* Optional end label */
        if (is_op(P, ":")) {
            advance(P);
            if (pk_ctyp(P) == TK_TOK_IDENT) advance(P);
        }
        return n;
    }
    /* else-if chain: the else branch is another if */
    if (pk_iskw(P, P->kw.kw_if))
        return pk_gif(P);

    /* Single module item without begin/end — just parse one stmt */
    return pk_stmt(P);
}

/* ---- Generate-if: if/else at module level ----
 * When inside a generate block, the if/else branches may contain
 * begin...end blocks with module items (always, wire, assign).
 * Standard pk_if delegates to pk_stmt which can't handle those. */

static uint32_t
pk_gif(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_GEN_IF);
    advance(P); /* eat 'if' */
    pk_eop(P, "(");
    uint32_t cond = pk_expr(P);
    pk_achld(P, n, cond);
    pk_eop(P, ")");

    uint32_t then_s = pk_gblk(P);
    pk_achld(P, n, then_s);

    if (pk_mkw(P, P->kw.kw_else)) {
        uint32_t else_s = pk_gblk(P);
        pk_achld(P, n, else_s);
    }

    return n;
}

static uint32_t
pk_if(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_IF);
    advance(P); /* eat 'if' */
    pk_eop(P, "(");
    uint32_t cond = pk_expr(P);
    pk_achld(P, n, cond);
    pk_eop(P, ")");

    uint32_t then_s = pk_stmt(P);
    pk_achld(P, n, then_s);

    if (pk_mkw(P, P->kw.kw_else)) {
        uint32_t else_s = pk_stmt(P);
        pk_achld(P, n, else_s);
    }

    return n;
}

static uint32_t
pk_case(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_CASE);
    advance(P); /* eat 'case' / 'casex' / 'casez' */
    pk_eop(P, "(");
    uint32_t sel = pk_expr(P);
    pk_achld(P, n, sel);
    pk_eop(P, ")");

    KA_GUARD(g, 1000);
    while (!pk_iskw(P, P->kw.endcase) && pk_ctyp(P) != TK_TOK_EOF && g--) {
        uint32_t item = pk_alloc(P, TK_AST_CASE_ITEM);

        if (pk_mkw(P, P->kw.kw_default)) {
            P->nodes[item].d.text.off = 0; /* mark as default */
            pk_mop(P, ":");
        } else {
            /* Case labels (comma-separated expressions) */
            KA_GUARD(gc, 100);
            while (gc--) {
                uint32_t lbl = pk_expr(P);
                pk_achld(P, item, lbl);
                if (!pk_mop(P, ",")) break;
            }
            pk_eop(P, ":");
        }

        /* Case body */
        uint32_t body = pk_stmt(P);
        pk_achld(P, item, body);
        pk_achld(P, n, item);
    }

    pk_ekw(P, P->kw.endcase);
    return n;
}

static uint32_t
pk_for(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_FOR);
    advance(P); /* eat 'for' */
    pk_eop(P, "(");

    /* Init */
    uint32_t init = pk_stmt(P);
    pk_achld(P, n, init);

    /* Condition */
    uint32_t cond = pk_expr(P);
    pk_achld(P, n, cond);
    pk_eop(P, ";");

    /* Increment: i = i + 1 or i++ or i--
     * C-style ++ is syntactic sugar for i = i + 1. */
    {
        uint32_t incr_lhs = pk_expr(P);
        if (is_op(P, "=")) {
            uint32_t asgn = pk_alloc(P, TK_AST_BLOCK_ASSIGN);
            advance(P); /* eat = */
            uint32_t incr_rhs = pk_expr(P);
            pk_achld(P, asgn, incr_lhs);
            pk_achld(P, asgn, incr_rhs);
            pk_achld(P, n, asgn);
        } else if (is_op(P, "++") || is_op(P, "--")) {
            /* i++ → i = i + 1, i-- → i = i - 1 */
            int is_inc = is_op(P, "++");
            uint32_t asgn = pk_alloc(P, TK_AST_BLOCK_ASSIGN);
            uint32_t bop  = pk_alloc(P, TK_AST_BINARY_OP);
            uint32_t one  = pk_alloc(P, TK_AST_INT_LIT);
            uint32_t lhs2 = pk_alloc(P, TK_AST_IDENT);
            advance(P); /* eat ++/-- */
            /* Clone the LHS ident for the RHS */
            P->nodes[lhs2].d.text.off = P->nodes[incr_lhs].d.text.off;
            P->nodes[lhs2].d.text.len = P->nodes[incr_lhs].d.text.len;
            /* Create literal 1 */
            {
                tk_lex_t *ml = (tk_lex_t *)P->lex;
                uint32_t off = ml->str_len;
                if (off + 2 <= ml->str_max) {
                    ml->strs[off] = '1';
                    ml->strs[off+1] = '\0';
                    ml->str_len += 2;
                }
                P->nodes[one].d.text.off = off;
                P->nodes[one].d.text.len = 1;
            }
            /* Find the + or - operator index */
            {
                uint32_t oi;
                for (oi = 0; oi < P->lex->n_op; oi++) {
                    const char *oc = P->lex->strs + P->lex->ops[oi].chars_off;
                    if (P->lex->ops[oi].chars_len == 1 &&
                        oc[0] == (is_inc ? '+' : '-'))
                        { P->nodes[bop].op = (uint16_t)oi; break; }
                }
            }
            pk_achld(P, bop, lhs2);
            pk_achld(P, bop, one);
            pk_achld(P, asgn, incr_lhs);
            pk_achld(P, asgn, bop);
            pk_achld(P, n, asgn);
        } else {
            pk_achld(P, n, incr_lhs);
        }
    }
    pk_eop(P, ")");

    /* Body */
    uint32_t body = pk_stmt(P);
    pk_achld(P, n, body);

    return n;
}

/* Parse a single statement */
static uint32_t
pk_stmt(tk_parse_t *P)
{
    /* Empty statement (bare semicolon). Perfectly legal in
     * SystemVerilog, commonly seen as `default: ;` in case
     * blocks. Like a rest in music: meaningful silence. */
    if (is_op(P, ";")) {
        advance(P);
        return 0;
    }

    /* begin...end block */
    if (pk_iskw(P, P->kw.begin))
        return pk_blk(P);

    /* if / else */
    if (pk_iskw(P, P->kw.kw_if))
        return pk_if(P);

    /* case / casex / casez */
    if (pk_iskw(P, P->kw.kw_case) || pk_iskw(P, P->kw.casex) ||
        pk_iskw(P, P->kw.casez))
        return pk_case(P);

    /* for loop */
    if (pk_iskw(P, P->kw.kw_for))
        return pk_for(P);

    /* Module instantiation in generate context:
     * IDENT IDENT ( -- two identifiers then paren */
    if (P->in_gen &&
        pk_ctyp(P) == TK_TOK_IDENT &&
        P->pos + 1 < P->n_tok &&
        P->tokens[P->pos + 1].type == TK_TOK_IDENT) {
        /* Delegate to pk_mbdy's instantiation handler
         * by wrapping in a temporary node */
        uint32_t inst = pk_alloc(P, TK_AST_INSTANCE);
        P->nodes[inst].d.text.off = cur(P)->off;
        P->nodes[inst].d.text.len = cur(P)->len;
        advance(P);
        uint32_t iname = pk_alloc(P, TK_AST_IDENT);
        P->nodes[iname].d.text.off = cur(P)->off;
        P->nodes[iname].d.text.len = cur(P)->len;
        advance(P);
        pk_achld(P, inst, iname);
        /* Port connections */
        if (is_op(P, "(")) {
            advance(P);
            KA_GUARD(gpc, 1000);
            while (!is_op(P, ")") && pk_ctyp(P) != TK_TOK_EOF && gpc--) {
                if (is_op(P, ".")) {
                    advance(P);
                    uint32_t conn = pk_alloc(P, TK_AST_CONN);
                    if (pk_ctyp(P) == TK_TOK_IDENT) {
                        P->nodes[conn].d.text.off = cur(P)->off;
                        P->nodes[conn].d.text.len = cur(P)->len;
                        advance(P);
                    }
                    if (is_op(P, "(")) {
                        advance(P);
                        if (!is_op(P, ")")) {
                            uint32_t val = pk_expr(P);
                            pk_achld(P, conn, val);
                        }
                        pk_eop(P, ")");
                    }
                    pk_achld(P, inst, conn);
                } else {
                    uint32_t val = pk_expr(P);
                    pk_achld(P, inst, val);
                }
                pk_mop(P, ",");
            }
            pk_eop(P, ")");
        }
        pk_mop(P, ";");
        return inst;
    }

    /* Continuous assign in generate context */
    if (P->in_gen && pk_iskw(P, P->kw.assign)) {
        uint32_t asgn = pk_alloc(P, TK_AST_ASSIGN);
        advance(P);
        uint32_t lhs = pk_expr(P);
        pk_achld(P, asgn, lhs);
        pk_eop(P, "=");
        uint32_t rhs = pk_expr(P);
        pk_achld(P, asgn, rhs);
        pk_mop(P, ";");
        return asgn;
    }

    /* Local variable declaration in block */
    if (pk_isty(P)) {
        uint32_t decl = pk_alloc(P, TK_AST_VAR_DECL);
        uint32_t ty = pk_type(P);
        pk_achld(P, decl, ty);
        if (pk_ctyp(P) == TK_TOK_IDENT) {
            uint32_t name = pk_alloc(P, TK_AST_IDENT);
            P->nodes[name].d.text.off = cur(P)->off;
            P->nodes[name].d.text.len = cur(P)->len;
            advance(P);
            pk_achld(P, decl, name);
        }
        /* Optional assignment */
        if (pk_mop(P, "=")) {
            uint32_t val = pk_expr(P);
            pk_achld(P, decl, val);
        }
        pk_mop(P, ";");
        return decl;
    }

    /* Assignment statement: lhs = expr; or lhs <= expr;
     * The Great SV Ambiguity: <= is both comparison and
     * non-blocking assignment. We suppress it as a binary
     * op while parsing the LHS, so it survives for us to
     * find here. Inside parens it's re-enabled (see pk_prim). */
    {
        P->no_le = 1;
        uint32_t lhs = pk_expr(P);
        P->no_le = 0;
        if (is_op(P, "<=")) {
            uint32_t n = pk_alloc(P, TK_AST_NONBLOCK);
            advance(P);
            uint32_t rhs = pk_expr(P);
            pk_achld(P, n, lhs);
            pk_achld(P, n, rhs);
            pk_mop(P, ";");
            return n;
        }
        if (is_op(P, "=")) {
            uint32_t n = pk_alloc(P, TK_AST_BLOCK_ASSIGN);
            advance(P);
            uint32_t rhs = pk_expr(P);
            pk_achld(P, n, lhs);
            pk_achld(P, n, rhs);
            pk_mop(P, ";");
            return n;
        }
        /* Bare expression (shouldn't happen in synthesis) */
        pk_mop(P, ";");
        return lhs;
    }
}

/* Parse statements until we hit end/endmodule/endcase/endgenerate/EOF */
static void
pk_stms(tk_parse_t *P, uint32_t parent)
{
    KA_GUARD(g, 100000);
    while (g--) {
        if (pk_ctyp(P) == TK_TOK_EOF) break;
        if (pk_iskw(P, P->kw.end) || pk_iskw(P, P->kw.endmodule) ||
            pk_iskw(P, P->kw.endcase) || pk_iskw(P, P->kw.endgenerate))
            break;

        uint32_t s = pk_stmt(P);
        if (s) pk_achld(P, parent, s);
    }
}

/* ---- Sensitivity List ---- */

static uint32_t
pk_sens(tk_parse_t *P)
{
    uint32_t n = pk_alloc(P, TK_AST_SENS_LIST);
    pk_eop(P, "@");

    /* @* (Verilog-2001 shorthand for @(*)) */
    if (is_op(P, "*")) {
        advance(P);
        return n;  /* wildcard, empty sens list = all signals */
    }

    pk_eop(P, "(");

    KA_GUARD(g, 100);
    while (!is_op(P, ")") && pk_ctyp(P) != TK_TOK_EOF && g--) {
        if (pk_iskw(P, P->kw.posedge) || pk_iskw(P, P->kw.negedge)) {
            uint32_t edge = pk_alloc(P, TK_AST_SENS_EDGE);
            P->nodes[edge].d.text.off = cur(P)->off;
            P->nodes[edge].d.text.len = cur(P)->len;
            advance(P);
            uint32_t sig = pk_expr(P);
            pk_achld(P, edge, sig);
            pk_achld(P, n, edge);
        } else if (is_op(P, "*")) {
            advance(P);  /* wildcard sensitivity */
        } else {
            uint32_t sig = pk_expr(P);
            pk_achld(P, n, sig);
        }
        pk_mkw(P, P->kw.kw_or);
        pk_mop(P, ",");
    }

    pk_eop(P, ")");
    return n;
}

/* ---- Module Items (ports, params, always, assign, etc.) ---- */

static void
pk_mbdy2(tk_parse_t *P, uint32_t mod, int stop_end)
{
    KA_GUARD(g, 100000);
    while (!pk_iskw(P, P->kw.endmodule) &&
           !pk_iskw(P, P->kw.endgenerate) &&
           !(stop_end && pk_iskw(P, P->kw.end)) &&
           pk_ctyp(P) != TK_TOK_EOF && g--) {

        /* Port / net declaration */
        if (pk_iskw(P, P->kw.input) || pk_iskw(P, P->kw.output) ||
            pk_iskw(P, P->kw.inout)) {
            uint32_t port = pk_alloc(P, TK_AST_PORT);
            P->nodes[port].d.text.off = cur(P)->off;  /* direction */
            P->nodes[port].d.text.len = cur(P)->len;
            advance(P);
            if (pk_isty(P)) {
                uint32_t ty = pk_type(P);
                pk_achld(P, port, ty);
            } else if (is_op(P, "[")) {
                /* Bare range: input [31:0] name */
                uint32_t ty = pk_alloc(P, TK_AST_TYPE_SPEC);
                advance(P);
                uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                uint32_t hi = pk_expr(P);
                pk_achld(P, rng, hi);
                pk_eop(P, ":");
                uint32_t lo = pk_expr(P);
                pk_achld(P, rng, lo);
                pk_eop(P, "]");
                pk_achld(P, ty, rng);
                pk_achld(P, port, ty);
            }
            /* Port name(s) */
            KA_GUARD(gp, 100);
            while (pk_ctyp(P) == TK_TOK_IDENT && gp--) {
                uint32_t name = pk_alloc(P, TK_AST_IDENT);
                P->nodes[name].d.text.off = cur(P)->off;
                P->nodes[name].d.text.len = cur(P)->len;
                advance(P);
                pk_achld(P, port, name);
                if (!pk_mop(P, ",")) break;
            }
            pk_mop(P, ";");
            pk_achld(P, mod, port);
            continue;
        }

        /* Parameter / localparam */
        if (pk_iskw(P, P->kw.parameter) || pk_iskw(P, P->kw.localparam)) {
            tk_ast_type_t pt = pk_iskw(P, P->kw.localparam) ?
                TK_AST_LOCALPARAM : TK_AST_PARAM;
            uint32_t param = pk_alloc(P, pt);
            advance(P);
            if (pk_isty(P)) {
                uint32_t ty = pk_type(P);
                pk_achld(P, param, ty);
            } else if (is_op(P, "[")) {
                /* Bare packed range: localparam [35:0] NAME = ... */
                uint32_t ty = pk_alloc(P, TK_AST_TYPE_SPEC);
                advance(P);
                uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                uint32_t hi = pk_expr(P);
                pk_achld(P, rng, hi);
                pk_eop(P, ":");
                uint32_t lo = pk_expr(P);
                pk_achld(P, rng, lo);
                pk_eop(P, "]");
                pk_achld(P, ty, rng);
                pk_achld(P, param, ty);
            }
            if (pk_ctyp(P) == TK_TOK_IDENT) {
                uint32_t name = pk_alloc(P, TK_AST_IDENT);
                P->nodes[name].d.text.off = cur(P)->off;
                P->nodes[name].d.text.len = cur(P)->len;
                advance(P);
                pk_achld(P, param, name);
            }
            if (pk_mop(P, "=")) {
                uint32_t val = pk_expr(P);
                pk_achld(P, param, val);
            }
            pk_mop(P, ";");
            pk_achld(P, mod, param);
            continue;
        }

        /* Wire / logic / reg declarations */
        if (pk_isty(P)) {
            uint32_t decl = pk_alloc(P, TK_AST_NET_DECL);
            uint32_t ty = pk_type(P);
            pk_achld(P, decl, ty);

            /* Variable name(s) */
            KA_GUARD(gn, 100);
            while (pk_ctyp(P) == TK_TOK_IDENT && gn--) {
                uint32_t name = pk_alloc(P, TK_AST_IDENT);
                P->nodes[name].d.text.off = cur(P)->off;
                P->nodes[name].d.text.len = cur(P)->len;
                advance(P);
                /* Unpacked array dimension [0:N-1] or [N] */
                if (is_op(P, "[")) {
                    advance(P);
                    uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                    uint32_t lo = pk_expr(P);
                    if (is_op(P, ":")) {
                        /* [lo:hi] range */
                        advance(P);
                        pk_achld(P, rng, lo);
                        uint32_t hi = pk_expr(P);
                        pk_achld(P, rng, hi);
                    } else {
                        /* [N] shorthand = [0:N-1].
                         * Just store N as single child — the
                         * elaborator treats it as depth. */
                        pk_achld(P, rng, lo);
                    }
                    pk_eop(P, "]");
                    P->nodes[name].next_sib = 0;
                    pk_achld(P, decl, name);
                    pk_achld(P, decl, rng);
                } else {
                    pk_achld(P, decl, name);
                }
                /* Inline assignment: wire foo = bar; */
                if (pk_mop(P, "=")) {
                    uint32_t val = pk_expr(P);
                    pk_achld(P, decl, val);
                }
                if (!pk_mop(P, ",")) break;
            }
            pk_mop(P, ";");
            pk_achld(P, mod, decl);
            continue;
        }

        /* Continuous assignment */
        if (pk_mkw(P, P->kw.assign)) {
            uint32_t asgn = pk_alloc(P, TK_AST_ASSIGN);
            uint32_t lhs = pk_expr(P);
            pk_achld(P, asgn, lhs);
            pk_eop(P, "=");
            uint32_t rhs = pk_expr(P);
            pk_achld(P, asgn, rhs);
            pk_mop(P, ";");
            pk_achld(P, mod, asgn);
            continue;
        }

        /* always_comb / always_ff / always_latch / always */
        if (pk_iskw(P, P->kw.always_comb) || pk_iskw(P, P->kw.always_ff) ||
            pk_iskw(P, P->kw.always_latch) || pk_iskw(P, P->kw.always)) {
            tk_ast_type_t at;
            if (pk_iskw(P, P->kw.always_comb))  at = TK_AST_ALWAYS_COMB;
            else if (pk_iskw(P, P->kw.always_ff))    at = TK_AST_ALWAYS_FF;
            else if (pk_iskw(P, P->kw.always_latch)) at = TK_AST_ALWAYS_LATCH;
            else                                      at = TK_AST_ALWAYS;

            uint32_t alw = pk_alloc(P, at);
            advance(P);

            /* Sensitivity list */
            if (is_op(P, "@")) {
                uint32_t sens = pk_sens(P);
                pk_achld(P, alw, sens);
            }

            /* Body */
            uint32_t body = pk_stmt(P);
            pk_achld(P, alw, body);
            pk_achld(P, mod, alw);
            continue;
        }

        /* Generate block — can contain all module-level items
         * (instantiations, always blocks, wires, etc.) not just
         * statements. So we reuse pk_mbdy which now stops
         * at endgenerate as well as endmodule. */
        if (pk_mkw(P, P->kw.generate)) {
            uint32_t gen = pk_alloc(P, TK_AST_GENERATE);
            uint8_t sav = P->in_gen;
            P->in_gen = 1;
            pk_mbdy(P, gen);
            P->in_gen = sav;
            pk_ekw(P, P->kw.endgenerate);
            pk_achld(P, mod, gen);
            continue;
        }

        /* genvar */
        if (pk_mkw(P, P->kw.genvar)) {
            uint32_t gv = pk_alloc(P, TK_AST_GENVAR);
            if (pk_ctyp(P) == TK_TOK_IDENT) {
                P->nodes[gv].d.text.off = cur(P)->off;
                P->nodes[gv].d.text.len = cur(P)->len;
                advance(P);
            }
            pk_mop(P, ";");
            pk_achld(P, mod, gv);
            continue;
        }

        /* initial block -- not synthesisable, skip entirely.
         * Like a "do not disturb" sign on a hotel room door:
         * we respect it and move on. */
        if (pk_mkw(P, P->kw.initial)) {
            /* Skip to matching 'end'. Track begin/end nesting. */
            int depth = 0;
            if (pk_iskw(P, P->kw.begin)) { depth = 1; advance(P); }
            KA_GUARD(gs, 100000);
            while (pk_ctyp(P) != TK_TOK_EOF && gs--) {
                if (pk_iskw(P, P->kw.begin)) { depth++; advance(P); }
                else if (pk_iskw(P, P->kw.end)) {
                    advance(P);
                    depth--;
                    if (depth <= 0) break;
                }
                else advance(P);
            }
            continue;
        }

        /* task ... endtask -- skip body for synthesis.
         * Tasks in synthesisable code are rare and weird.
         * PicoRV32 has one and it's literally empty. */
        if (pk_mkw(P, P->kw.task)) {
            KA_GUARD(gs, 100000);
            while (!pk_iskw(P, P->kw.endtask) &&
                   pk_ctyp(P) != TK_TOK_EOF && gs--)
                advance(P);
            pk_mkw(P, P->kw.endtask);
            continue;
        }

        /* function ... endfunction -- skip for now.
         * Constant functions needed for elaboration will
         * get proper handling in Tier 1. */
        if (pk_mkw(P, P->kw.kw_function)) {
            KA_GUARD(gs, 100000);
            while (!pk_iskw(P, P->kw.endfunction) &&
                   pk_ctyp(P) != TK_TOK_EOF && gs--)
                advance(P);
            pk_mkw(P, P->kw.endfunction);
            continue;
        }

        /* typedef -- register the name so we recognise it as
         * a type later. The name is always the last identifier
         * before the semicolon. We skip the body for now but
         * capture that precious name like a customs officer
         * stamping a passport without reading the luggage. */
        if (pk_mkw(P, P->kw.kw_typedef)) {
            uint32_t td = pk_alloc(P, TK_AST_TYPEDEF);
            uint32_t last_id_off = 0;
            uint16_t last_id_len = 0;
            int brace_depth = 0;
            /* Skip typedef body, tracking braces for struct/enum.
             * typedef struct packed { logic a; logic b; } name_t;
             * The last IDENT before ; is the type name. */
            KA_GUARD(gs, 1000);
            while (pk_ctyp(P) != TK_TOK_EOF && gs--) {
                if (is_op(P, "{")) { brace_depth++; advance(P); continue; }
                if (is_op(P, "}")) { brace_depth--; advance(P); continue; }
                if (is_op(P, ";") && brace_depth <= 0) break;
                if (pk_ctyp(P) == TK_TOK_IDENT && brace_depth == 0) {
                    last_id_off = cur(P)->off;
                    last_id_len = cur(P)->len;
                }
                advance(P);
            }
            if (last_id_len > 0) {
                P->nodes[td].d.text.off = last_id_off;
                P->nodes[td].d.text.len = last_id_len;
                pk_rtm(P, last_id_off, last_id_len);
            }
            pk_mop(P, ";");
            pk_achld(P, mod, td);
            continue;
        }

        /* if/else at module level (generate if).
         * Uses pk_gif so begin...end bodies parse as module items
         * (always blocks, wires, assigns) not just statements. */
        if (pk_iskw(P, P->kw.kw_if)) {
            uint32_t ifs = P->in_gen ? pk_gif(P) : pk_if(P);
            pk_achld(P, mod, ifs);
            continue;
        }

        /* for at module level (generate for) */
        if (pk_iskw(P, P->kw.kw_for)) {
            uint32_t fors = pk_for(P);
            pk_achld(P, mod, fors);
            continue;
        }

        /* begin/end at module level (generate block body) */
        if (pk_iskw(P, P->kw.begin)) {
            uint32_t blk = pk_alloc(P, TK_AST_BEGIN_END);
            pk_ekw(P, P->kw.begin);
            if (is_op(P, ":")) {
                advance(P);
                if (pk_ctyp(P) == TK_TOK_IDENT) {
                    P->nodes[blk].d.text.off = cur(P)->off;
                    P->nodes[blk].d.text.len = cur(P)->len;
                    advance(P);
                }
            }
            pk_mbdy(P, blk);
            pk_ekw(P, P->kw.end);
            if (is_op(P, ":")) {
                advance(P);
                if (pk_ctyp(P) == TK_TOK_IDENT) advance(P);
            }
            pk_achld(P, mod, blk);
            continue;
        }

        /* Module instantiation: modname [#(...)] instname ( ... );
         * Detected when we see IDENT IDENT or IDENT #(. */
        if (pk_ctyp(P) == TK_TOK_IDENT &&
            P->pos + 1 < P->n_tok &&
            (P->tokens[P->pos + 1].type == TK_TOK_IDENT ||
             (P->tokens[P->pos + 1].type == TK_TOK_OP))) {
            /* Peek: is next token an ident or #? If #, check for ( after */
            int is_inst = 0;
            if (P->tokens[P->pos + 1].type == TK_TOK_IDENT)
                is_inst = 1;
            else {
                const char *nxt = P->lex->strs + P->tokens[P->pos + 1].off;
                if (nxt[0] == '#' && P->pos + 2 < P->n_tok) {
                    const char *nxt2 = P->lex->strs + P->tokens[P->pos + 2].off;
                    if (nxt2[0] == '(') is_inst = 1;
                }
            }

          if (is_inst) {
            uint32_t inst = pk_alloc(P, TK_AST_INSTANCE);
            /* Module type name */
            P->nodes[inst].d.text.off = cur(P)->off;
            P->nodes[inst].d.text.len = cur(P)->len;
            advance(P);

            /* Parameter override #(...) BEFORE instance name */
            if (is_op(P, "#")) {
                advance(P);
                if (is_op(P, "(")) {
                    int pdepth = 1;
                    advance(P);
                    KA_GUARD(gpo, 10000);
                    while (pdepth > 0 && pk_ctyp(P) != TK_TOK_EOF && gpo--) {
                        if (is_op(P, "(")) pdepth++;
                        else if (is_op(P, ")")) pdepth--;
                        if (pdepth > 0) advance(P);
                    }
                    if (is_op(P, ")")) advance(P);
                }
            }

            /* Instance name (after optional #(...)) */
            if (pk_ctyp(P) == TK_TOK_IDENT) {
                uint32_t iname = pk_alloc(P, TK_AST_IDENT);
                P->nodes[iname].d.text.off = cur(P)->off;
                P->nodes[iname].d.text.len = cur(P)->len;
                advance(P);
                pk_achld(P, inst, iname);
            }

            /* Port connections (...) */
            if (is_op(P, "(")) {
                advance(P);
                KA_GUARD(gpc, 1000);
                while (!is_op(P, ")") && pk_ctyp(P) != TK_TOK_EOF && gpc--) {
                    if (is_op(P, ".")) {
                        /* Named connection: .port(expr) */
                        advance(P);
                        uint32_t conn = pk_alloc(P, TK_AST_CONN);
                        if (pk_ctyp(P) == TK_TOK_IDENT) {
                            P->nodes[conn].d.text.off = cur(P)->off;
                            P->nodes[conn].d.text.len = cur(P)->len;
                            advance(P);
                        }
                        if (is_op(P, "(")) {
                            advance(P);
                            if (!is_op(P, ")")) {
                                uint32_t val = pk_expr(P);
                                pk_achld(P, conn, val);
                            }
                            pk_eop(P, ")");
                        }
                        pk_achld(P, inst, conn);
                    } else {
                        /* Positional connection */
                        uint32_t val = pk_expr(P);
                        pk_achld(P, inst, val);
                    }
                    pk_mop(P, ",");
                }
                pk_eop(P, ")");
            }
            pk_mop(P, ";");
            pk_achld(P, mod, inst);
            continue;
          }
        }

        /* Preprocessor directives — skip silently.
         * Without a preprocessor, ifdef/endif blocks get
         * tokenised but can't be evaluated. Just eat them. */
        if (pk_ctyp(P) == TK_TOK_PREPROC) {
            advance(P);
            /* Skip any arguments on the same logical line */
            KA_GUARD(gpp, 1000);
            while (pk_ctyp(P) != TK_TOK_EOF && gpp--) {
                /* Stop at next statement-level construct */
                if (is_op(P, ";")) break;
                if (pk_ctyp(P) == TK_TOK_PREPROC) break;
                if (pk_iskw(P, P->kw.module) ||
                    pk_iskw(P, P->kw.endmodule) ||
                    pk_iskw(P, P->kw.always) ||
                    pk_iskw(P, P->kw.always_comb) ||
                    pk_iskw(P, P->kw.always_ff) ||
                    pk_iskw(P, P->kw.assign) ||
                    pk_iskw(P, P->kw.input) ||
                    pk_iskw(P, P->kw.output) ||
                    pk_isty(P)) break;
                advance(P);
            }
            continue;
        }

        /* System task at module level (e.g. $display in ifdef blocks) */
        if (pk_ctyp(P) == TK_TOK_SYSTASK) {
            /* Skip the call and arguments */
            advance(P);
            if (is_op(P, "(")) {
                int depth = 1;
                advance(P);
                KA_GUARD(gst, 10000);
                while (depth > 0 && pk_ctyp(P) != TK_TOK_EOF && gst--) {
                    if (is_op(P, "(")) depth++;
                    else if (is_op(P, ")")) depth--;
                    advance(P);
                }
            }
            pk_mop(P, ";");
            continue;
        }

        /* Formal verification: restrict/assume/cover property — skip.
         * These are IEEE 1800 formal constructs that aren't
         * synthesisable. Eat to the next semicolon. */
        if (pk_ctyp(P) == TK_TOK_KWD) {
            const char *kwt = P->lex->strs + cur(P)->off;
            if (memcmp(kwt, "restrict", 8) == 0 ||
                memcmp(kwt, "assume", 6) == 0 ||
                memcmp(kwt, "cover", 5) == 0) {
                KA_GUARD(gfv, 1000);
                while (!is_op(P, ";") &&
                       pk_ctyp(P) != TK_TOK_EOF && gfv--)
                    advance(P);
                pk_mop(P, ";");
                continue;
            }
        }

        /* Unknown token -- skip with error recovery */
        if (P->n_err < TK_MAX_ERRORS) {
            const tk_token_t *t = cur(P);
            tk_err_t *e = &P->errors[P->n_err++];
            e->line = t->line;
            e->col  = t->col;
            snprintf(e->msg, sizeof(e->msg),
                     "unexpected in module body: '%.*s'",
                     t->len > 30 ? 30 : t->len,
                     P->lex->strs + t->off);
        }
        pk_sync(P);
    }
}

/* pk_mbdy: wrapper without end-stop (original API) */
static void
pk_mbdy(tk_parse_t *P, uint32_t mod)
{
    pk_mbdy2(P, mod, 0);
}

/* ---- Module Declaration ---- */

static uint32_t
pk_mod(tk_parse_t *P)
{
    uint32_t mod = pk_alloc(P, TK_AST_MODULE);
    pk_ekw(P, P->kw.module);

    /* Module name */
    if (pk_ctyp(P) == TK_TOK_IDENT) {
        P->nodes[mod].d.text.off = cur(P)->off;
        P->nodes[mod].d.text.len = cur(P)->len;
        advance(P);
    }

    /* Parameter port list #(...) */
    if (is_op(P, "#")) {
        advance(P);
        if (is_op(P, "(")) {
            advance(P);
            KA_GUARD(gp, 100);
            while (!is_op(P, ")") && pk_ctyp(P) != TK_TOK_EOF && gp--) {
                if (pk_mkw(P, P->kw.parameter) ||
                pk_mkw(P, P->kw.localparam)) {
                    /* skip */
                }
                uint32_t param = pk_alloc(P, TK_AST_PARAM);

                /* Optional type (logic, integer, etc.) */
                if (pk_isty(P)) {
                    uint32_t ty = pk_type(P);
                    pk_achld(P, param, ty);
                }
                /* Bare packed range: parameter [0:0] NAME = ... */
                else if (is_op(P, "[")) {
                    uint32_t ty = pk_alloc(P, TK_AST_TYPE_SPEC);
                    advance(P);
                    uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                    uint32_t hi = pk_expr(P);
                    pk_achld(P, rng, hi);
                    pk_eop(P, ":");
                    uint32_t lo = pk_expr(P);
                    pk_achld(P, rng, lo);
                    pk_eop(P, "]");
                    pk_achld(P, ty, rng);
                    pk_achld(P, param, ty);
                }

                if (pk_ctyp(P) == TK_TOK_IDENT) {
                    uint32_t name = pk_alloc(P, TK_AST_IDENT);
                    P->nodes[name].d.text.off = cur(P)->off;
                    P->nodes[name].d.text.len = cur(P)->len;
                    advance(P);
                    pk_achld(P, param, name);
                }
                if (pk_mop(P, "=")) {
                    uint32_t val = pk_expr(P);
                    pk_achld(P, param, val);
                }
                pk_achld(P, mod, param);
                pk_mop(P, ",");
            }
            pk_eop(P, ")");
        }
    }

    /* ANSI port list (...) */
    if (is_op(P, "(")) {
        advance(P);
        KA_GUARD(gp, 200);
        while (!is_op(P, ")") && pk_ctyp(P) != TK_TOK_EOF && gp--) {
            if (pk_iskw(P, P->kw.input) || pk_iskw(P, P->kw.output) ||
                pk_iskw(P, P->kw.inout)) {
                uint32_t port = pk_alloc(P, TK_AST_PORT);
                P->nodes[port].d.text.off = cur(P)->off;
                P->nodes[port].d.text.len = cur(P)->len;
                advance(P);
                if (pk_isty(P)) {
                    uint32_t ty = pk_type(P);
                    pk_achld(P, port, ty);
                } else if (is_op(P, "[")) {
                    /* Bare range after direction: input [31:0] name */
                    uint32_t ty = pk_alloc(P, TK_AST_TYPE_SPEC);
                    advance(P);
                    uint32_t rng = pk_alloc(P, TK_AST_RANGE);
                    uint32_t hi = pk_expr(P);
                    pk_achld(P, rng, hi);
                    pk_eop(P, ":");
                    uint32_t lo = pk_expr(P);
                    pk_achld(P, rng, lo);
                    pk_eop(P, "]");
                    pk_achld(P, ty, rng);
                    pk_achld(P, port, ty);
                }
                if (pk_ctyp(P) == TK_TOK_IDENT) {
                    uint32_t name = pk_alloc(P, TK_AST_IDENT);
                    P->nodes[name].d.text.off = cur(P)->off;
                    P->nodes[name].d.text.len = cur(P)->len;
                    advance(P);
                    pk_achld(P, port, name);
                }
                pk_achld(P, mod, port);
            } else if (pk_ctyp(P) == TK_TOK_IDENT) {
                /* Non-ANSI port name */
                uint32_t port = pk_alloc(P, TK_AST_PORT);
                P->nodes[port].d.text.off = cur(P)->off;
                P->nodes[port].d.text.len = cur(P)->len;
                advance(P);
                pk_achld(P, mod, port);
            }
            pk_mop(P, ",");
        }
        pk_eop(P, ")");
    }

    pk_eop(P, ";");

    /* Module body */
    pk_mbdy(P, mod);

    pk_ekw(P, P->kw.endmodule);
    return mod;
}

/* ---- Top Level ---- */

int
tk_pinit(tk_parse_t *P, const tk_lex_t *L)
{
    if (!P || !L) return -1;

    memset(P, 0, sizeof(*P));
    P->tokens  = L->tokens;
    P->n_tok   = L->n_tok;
    P->lex     = L;
    P->pos     = 0;

    P->nodes = (tk_node_t *)calloc(TK_MAX_NODES, sizeof(tk_node_t));
    if (!P->nodes) return -1;
    P->max_node = TK_MAX_NODES;
    P->n_node = 1; /* index 0 = sentinel */

    /* Pre-compute keyword IDs. One-time cost at parser init;
     * saves thousands of strcmp calls during parsing.
     * Like pre-computing a truth table instead of evaluating
     * the boolean expression every clock cycle. */
    P->kw.module      = pk_kwfn(L, "module");
    P->kw.endmodule   = pk_kwfn(L, "endmodule");
    P->kw.begin       = pk_kwfn(L, "begin");
    P->kw.end         = pk_kwfn(L, "end");
    P->kw.input       = pk_kwfn(L, "input");
    P->kw.output      = pk_kwfn(L, "output");
    P->kw.inout       = pk_kwfn(L, "inout");
    P->kw.parameter   = pk_kwfn(L, "parameter");
    P->kw.localparam  = pk_kwfn(L, "localparam");
    P->kw.assign      = pk_kwfn(L, "assign");
    P->kw.always      = pk_kwfn(L, "always");
    P->kw.always_comb = pk_kwfn(L, "always_comb");
    P->kw.always_ff   = pk_kwfn(L, "always_ff");
    P->kw.always_latch= pk_kwfn(L, "always_latch");
    P->kw.kw_if       = pk_kwfn(L, "if");
    P->kw.kw_else     = pk_kwfn(L, "else");
    P->kw.kw_case     = pk_kwfn(L, "case");
    P->kw.casex       = pk_kwfn(L, "casex");
    P->kw.casez       = pk_kwfn(L, "casez");
    P->kw.endcase     = pk_kwfn(L, "endcase");
    P->kw.kw_for      = pk_kwfn(L, "for");
    P->kw.generate    = pk_kwfn(L, "generate");
    P->kw.endgenerate = pk_kwfn(L, "endgenerate");
    P->kw.genvar      = pk_kwfn(L, "genvar");
    P->kw.kw_default  = pk_kwfn(L, "default");
    P->kw.posedge     = pk_kwfn(L, "posedge");
    P->kw.negedge     = pk_kwfn(L, "negedge");
    P->kw.kw_or       = pk_kwfn(L, "or");
    P->kw.kw_typedef  = pk_kwfn(L, "typedef");
    P->kw.kw_enum     = pk_kwfn(L, "enum");
    P->kw.kw_struct   = pk_kwfn(L, "struct");
    P->kw.kw_union    = pk_kwfn(L, "union");
    P->kw.packed      = pk_kwfn(L, "packed");
    P->kw.signed_kw   = pk_kwfn(L, "signed");
    P->kw.unsigned_kw = pk_kwfn(L, "unsigned");
    P->kw.logic       = pk_kwfn(L, "logic");
    P->kw.wire        = pk_kwfn(L, "wire");
    P->kw.reg         = pk_kwfn(L, "reg");
    P->kw.bit         = pk_kwfn(L, "bit");
    P->kw.integer     = pk_kwfn(L, "integer");
    P->kw.kw_int      = pk_kwfn(L, "int");
    P->kw.byte_kw     = pk_kwfn(L, "byte");
    P->kw.shortint    = pk_kwfn(L, "shortint");
    P->kw.longint     = pk_kwfn(L, "longint");
    P->kw.real        = pk_kwfn(L, "real");
    P->kw.shortreal   = pk_kwfn(L, "shortreal");
    P->kw.realtime    = pk_kwfn(L, "realtime");
    P->kw.time_kw     = pk_kwfn(L, "time");
    P->kw.unique      = pk_kwfn(L, "unique");
    P->kw.priority    = pk_kwfn(L, "priority");
    P->kw.initial     = pk_kwfn(L, "initial");
    P->kw.task        = pk_kwfn(L, "task");
    P->kw.endtask     = pk_kwfn(L, "endtask");
    P->kw.kw_function = pk_kwfn(L, "function");
    P->kw.endfunction = pk_kwfn(L, "endfunction");

    return 0;
}

int
tk_parse(tk_parse_t *P)
{
    uint32_t root = pk_alloc(P, TK_AST_ROOT);

    KA_GUARD(g, 10000);
    while (pk_ctyp(P) != TK_TOK_EOF && g--) {
        /* Top-level typedef */
        if (pk_iskw(P, P->kw.kw_typedef)) {
            uint32_t td = pk_alloc(P, TK_AST_TYPEDEF);
            uint32_t last_id_off = 0;
            uint16_t last_id_len = 0;
            advance(P);
            KA_GUARD(gs, 1000);
            while (!is_op(P, ";") && pk_ctyp(P) != TK_TOK_EOF && gs--) {
                if (pk_ctyp(P) == TK_TOK_IDENT) {
                    last_id_off = cur(P)->off;
                    last_id_len = cur(P)->len;
                }
                advance(P);
            }
            if (last_id_len > 0) {
                P->nodes[td].d.text.off = last_id_off;
                P->nodes[td].d.text.len = last_id_len;
                pk_rtm(P, last_id_off, last_id_len);
            }
            pk_mop(P, ";");
            pk_achld(P, root, td);
            continue;
        }

        /* Module */
        if (pk_iskw(P, P->kw.module)) {
            uint32_t mod = pk_mod(P);
            pk_achld(P, root, mod);
            continue;
        }

        /* Skip unknown top-level tokens */
        advance(P);
    }

    return (P->n_err > 0) ? -1 : 0;
}

void
tk_pfree(tk_parse_t *P)
{
    if (!P) return;
    free(P->nodes);
    memset(P, 0, sizeof(*P));
}

/* ---- AST Dump ---- */

const char *
tk_aststr(tk_ast_type_t t)
{
    switch (t) {
    case TK_AST_ROOT:         return "ROOT";
    case TK_AST_MODULE:       return "MODULE";
    case TK_AST_PORT:         return "PORT";
    case TK_AST_PARAM:        return "PARAM";
    case TK_AST_LOCALPARAM:   return "LOCALPARAM";
    case TK_AST_TYPEDEF:      return "TYPEDEF";
    case TK_AST_ENUM_DEF:     return "ENUM";
    case TK_AST_STRUCT_DEF:   return "STRUCT";
    case TK_AST_MEMBER:       return "MEMBER";
    case TK_AST_TYPE_SPEC:    return "TYPE";
    case TK_AST_NET_DECL:     return "NET_DECL";
    case TK_AST_VAR_DECL:     return "VAR_DECL";
    case TK_AST_ASSIGN:       return "ASSIGN";
    case TK_AST_BLOCK_ASSIGN: return "BLOCK_ASSIGN";
    case TK_AST_NONBLOCK:     return "NONBLOCK";
    case TK_AST_ALWAYS_COMB:  return "ALWAYS_COMB";
    case TK_AST_ALWAYS_FF:    return "ALWAYS_FF";
    case TK_AST_ALWAYS_LATCH: return "ALWAYS_LATCH";
    case TK_AST_ALWAYS:       return "ALWAYS";
    case TK_AST_SENS_LIST:    return "SENS_LIST";
    case TK_AST_SENS_EDGE:    return "SENS_EDGE";
    case TK_AST_IF:           return "IF";
    case TK_AST_CASE:         return "CASE";
    case TK_AST_CASE_ITEM:    return "CASE_ITEM";
    case TK_AST_FOR:          return "FOR";
    case TK_AST_WHILE:        return "WHILE";
    case TK_AST_BEGIN_END:    return "BEGIN_END";
    case TK_AST_GENERATE:     return "GENERATE";
    case TK_AST_GENVAR:       return "GENVAR";
    case TK_AST_GEN_FOR:      return "GEN_FOR";
    case TK_AST_GEN_IF:       return "GEN_IF";
    case TK_AST_IDENT:        return "IDENT";
    case TK_AST_INT_LIT:      return "INT_LIT";
    case TK_AST_REAL_LIT:     return "REAL_LIT";
    case TK_AST_STR_LIT:      return "STR_LIT";
    case TK_AST_BINARY_OP:    return "BINOP";
    case TK_AST_UNARY_OP:     return "UNOP";
    case TK_AST_TERNARY:      return "TERNARY";
    case TK_AST_CONCAT:       return "CONCAT";
    case TK_AST_REPLICATE:    return "REPLICATE";
    case TK_AST_INDEX:        return "INDEX";
    case TK_AST_RANGE:        return "RANGE";
    case TK_AST_MEMBER_ACC:   return "MEMBER";
    case TK_AST_CALL:         return "CALL";
    case TK_AST_CAST:         return "CAST";
    case TK_AST_INSTANCE:     return "INSTANCE";
    case TK_AST_CONN:         return "CONN";
    case TK_AST_COUNT:        return "?";
    default:                   return "?";
    }
}

void
tk_pdump(const tk_parse_t *P, uint32_t node, int depth)
{
    uint32_t c;
    int i;

    if (node == 0 || KA_CHK(node, P->n_node)) return;

    const tk_node_t *n = &P->nodes[node];

    for (i = 0; i < depth; i++) printf("  ");
    printf("%s", tk_aststr(n->type));

    /* Print text payload for named nodes */
    if (n->d.text.len > 0 && n->d.text.off > 0) {
        const char *text = P->lex->strs + n->d.text.off;
        printf(" '%.*s'", n->d.text.len > 40 ? 40 : (int)n->d.text.len, text);
    }

    printf("  [%u:%u]\n", n->line, n->col);

    /* Recurse children */
    c = n->first_child;
    KA_GUARD(g, 10000);
    while (c != 0 && g--) {
        tk_pdump(P, c, depth + 1);
        c = P->nodes[c].next_sib;
    }
}
