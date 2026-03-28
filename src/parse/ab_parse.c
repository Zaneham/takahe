/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * ab_parse.c -- ABEL-HDL parser for Takahe
 *
 * Produces the same AST node types as the SystemVerilog and
 * VHDL frontends so the entire backend works unchanged.
 *
 * ABEL structure:
 *   MODULE name
 *   TITLE 'text'
 *   device DEVICE 'type';
 *   DECLARATIONS pin/node/constant
 *   EQUATIONS | TRUTH_TABLE | STATE_DIAGRAM
 *   TEST_VECTORS
 *   END name
 *
 * Synario ABEL-HDL Reference, Data I/O, 1995.
 */

#include "takahe.h"
#include <ctype.h>

/* ---- Keyword ID cache ---- */

typedef struct {
    uint16_t kw_module;
    uint16_t kw_title;
    uint16_t kw_decl;
    uint16_t kw_device;
    uint16_t kw_pin;
    uint16_t kw_node;
    uint16_t kw_istype;
    uint16_t kw_eqns;
    uint16_t kw_tt;
    uint16_t kw_sd;
    uint16_t kw_sr;
    uint16_t kw_state;
    uint16_t kw_tv;
    uint16_t kw_end;
    uint16_t kw_if;
    uint16_t kw_then;
    uint16_t kw_else;
    uint16_t kw_case;
    uint16_t kw_endcase;
    uint16_t kw_when;
    uint16_t kw_with;
    uint16_t kw_endwith;
    uint16_t kw_goto;
    uint16_t kw_fuses;
    uint16_t kw_macro;
    uint16_t kw_ext;
    uint16_t kw_intfc;
    uint16_t kw_fblk;
    uint16_t kw_lib;
    uint16_t kw_trace;
    uint16_t kw_prop;
    uint16_t kw_opts;
    uint16_t kw_in;
} ab_kwc_t;

/* ---- Parser context (stack-local, wraps tk_parse_t) ---- */

typedef struct {
    tk_parse_t *P;
    const tk_lex_t *L;
    ab_kwc_t kw;
} ab_ctx_t;

/* ---- Keyword finder ---- */

static uint16_t
ab_kwfn(const tk_lex_t *L, const char *name)
{
    uint32_t i;
    uint16_t nlen = (uint16_t)strlen(name);
    for (i = 0; i < L->n_kwd; i++) {
        if (L->kwds[i].name_len == nlen &&
            memcmp(L->strs + L->kwds[i].name_off, name, nlen) == 0)
            return (uint16_t)i;
    }
    return TK_KW_NONE;
}

/* ---- Token helpers ---- */

static const tk_token_t *
ab_cur(const ab_ctx_t *C)
{
    if (C->P->pos >= C->P->n_tok)
        return &C->P->tokens[C->P->n_tok - 1];
    return &C->P->tokens[C->P->pos];
}

static tk_toktype_t
ab_ctyp(const ab_ctx_t *C) { return ab_cur(C)->type; }

static uint16_t
ab_csub(const ab_ctx_t *C) { return ab_cur(C)->sub; }

static int
ab_eof(const ab_ctx_t *C) { return ab_ctyp(C) == TK_TOK_EOF; }

static void
ab_adv(ab_ctx_t *C) { if (C->P->pos < C->P->n_tok) C->P->pos++; }

static int
ab_iskw(const ab_ctx_t *C, uint16_t kwid)
{
    return ab_ctyp(C) == TK_TOK_KWD && ab_csub(C) == kwid;
}

static int
ab_isop(const ab_ctx_t *C, const char *name)
{
    if (ab_ctyp(C) != TK_TOK_OP) return 0;
    const char *opn = C->L->strs + C->L->ops[ab_csub(C)].name_off;
    return strcmp(opn, name) == 0;
}

/* ---- AST node allocation ---- */

static uint32_t
ab_node(ab_ctx_t *C, tk_ast_type_t type, uint32_t tok_idx)
{
    tk_parse_t *P = C->P;
    uint32_t ni;
    const tk_token_t *t;

    if (P->n_node >= TK_MAX_NODES) return 0;

    ni = P->n_node++;
    memset(&P->nodes[ni], 0, sizeof(P->nodes[ni]));
    P->nodes[ni].type = type;

    /* store source location from the token */
    t = (tok_idx < P->n_tok) ? &P->tokens[tok_idx] : NULL;
    if (t) {
        P->nodes[ni].line = t->line;
        P->nodes[ni].col  = t->col;
    }
    return ni;
}

static void
ab_achld(ab_ctx_t *C, uint32_t parent, uint32_t child)
{
    tk_parse_t *P = C->P;
    tk_node_t *pn;

    if (parent == 0 || child == 0) return;
    if (parent >= P->n_node || child >= P->n_node) return;

    pn = &P->nodes[parent];
    P->nodes[child].next_sib = 0;

    if (pn->first_child == 0) {
        pn->first_child = child;
        pn->last_child  = child;
    } else {
        P->nodes[pn->last_child].next_sib = child;
        pn->last_child = child;
    }
}

static void
ab_err(ab_ctx_t *C, const char *msg)
{
    tk_parse_t *P = C->P;
    if (P->n_err >= TK_MAX_ERRORS) return;
    tk_err_t *e = &P->errors[P->n_err++];
    e->line = ab_cur(C)->line;
    e->col  = ab_cur(C)->col;
    snprintf(e->msg, sizeof(e->msg), "%s", msg);
}

/* ---- Skip to semicolon (error recovery) ---- */

static void
ab_skipsemi(ab_ctx_t *C)
{
    KA_GUARD(g, 10000);
    while (!ab_eof(C) && !ab_isop(C, "semi") && g--)
        ab_adv(C);
    if (ab_isop(C, "semi")) ab_adv(C);
}

/* ---- Expression parser ----
 * Precedence climbing per the ABEL operator priority table.
 * Priority 1 (highest): ! -
 * Priority 2: & << >> * / %
 * Priority 3: + - # $ !$
 * Priority 4 (lowest): == != < <= > >= */

static uint32_t ab_expr(ab_ctx_t *C, int minp);

static int
ab_oprec(const ab_ctx_t *C)
{
    if (ab_ctyp(C) != TK_TOK_OP) return -1;
    const char *nm = C->L->strs + C->L->ops[ab_csub(C)].name_off;

    if (strcmp(nm, "band") == 0)  return 2;
    if (strcmp(nm, "shl") == 0)   return 2;
    if (strcmp(nm, "shr") == 0)   return 2;
    if (strcmp(nm, "mul") == 0)   return 2;
    if (strcmp(nm, "div") == 0)   return 2;
    if (strcmp(nm, "mod") == 0)   return 2;
    if (strcmp(nm, "add") == 0)   return 3;
    if (strcmp(nm, "sub") == 0)   return 3;
    if (strcmp(nm, "bor") == 0)   return 3;
    if (strcmp(nm, "bxor") == 0)  return 3;
    if (strcmp(nm, "bxnor") == 0) return 3;
    if (strcmp(nm, "eq") == 0)    return 4;
    if (strcmp(nm, "neq") == 0)   return 4;
    if (strcmp(nm, "lt") == 0)    return 4;
    if (strcmp(nm, "le") == 0)    return 4;
    if (strcmp(nm, "gt") == 0)    return 4;
    if (strcmp(nm, "ge") == 0)    return 4;

    return -1;
}

static uint32_t
ab_primary(ab_ctx_t *C)
{
    /* unary ! */
    if (ab_isop(C, "not")) {
        uint32_t tok = C->P->pos;
        ab_adv(C);
        uint32_t operand = ab_primary(C);
        uint32_t nd = ab_node(C, TK_AST_UNARY_OP, tok);
        ab_achld(C, nd, operand);
        return nd;
    }

    /* unary - (negate) */
    if (ab_isop(C, "negate") || ab_isop(C, "sub")) {
        uint32_t tok = C->P->pos;
        ab_adv(C);
        uint32_t operand = ab_primary(C);
        uint32_t nd = ab_node(C, TK_AST_UNARY_OP, tok);
        ab_achld(C, nd, operand);
        return nd;
    }

    /* parenthesised expression */
    if (ab_isop(C, "lparen")) {
        ab_adv(C);
        uint32_t expr = ab_expr(C, 1);
        if (ab_isop(C, "rparen")) ab_adv(C);
        return expr;
    }

    /* set literal [a, b, c] */
    if (ab_isop(C, "lbracket")) {
        uint32_t tok = C->P->pos;
        uint32_t nd = ab_node(C, TK_AST_CONCAT, tok);
        ab_adv(C);
        KA_GUARD(gs, 256);
        while (!ab_eof(C) && !ab_isop(C, "rbracket") && gs--) {
            ab_achld(C, nd, ab_expr(C, 1));
            if (ab_isop(C, "comma")) ab_adv(C);
            /* range operator .. */
            else if (ab_isop(C, "dotdot")) ab_adv(C);
            else break;
        }
        if (ab_isop(C, "rbracket")) ab_adv(C);
        return nd;
    }

    /* integer literal */
    if (ab_ctyp(C) == TK_TOK_INT_LIT) {
        uint32_t nd = ab_node(C, TK_AST_INT_LIT, C->P->pos);
        C->P->nodes[nd].d.text.off = ab_cur(C)->off;
        C->P->nodes[nd].d.text.len = ab_cur(C)->len;
        ab_adv(C);
        return nd;
    }

    /* string literal */
    if (ab_ctyp(C) == TK_TOK_STR_LIT) {
        uint32_t nd = ab_node(C, TK_AST_STR_LIT, C->P->pos);
        C->P->nodes[nd].d.text.off = ab_cur(C)->off;
        C->P->nodes[nd].d.text.len = ab_cur(C)->len;
        ab_adv(C);
        return nd;
    }

    /* identifier, possibly with dot extension or set index */
    if (ab_ctyp(C) == TK_TOK_IDENT) {
        uint32_t nd = ab_node(C, TK_AST_IDENT, C->P->pos);
        C->P->nodes[nd].d.text.off = ab_cur(C)->off;
        C->P->nodes[nd].d.text.len = ab_cur(C)->len;
        ab_adv(C);

        /* dot extension: signal.CLK, signal.OE etc */
        if (ab_isop(C, "dot") && !ab_eof(C)) {
            uint32_t acc = ab_node(C, TK_AST_MEMBER_ACC, C->P->pos);
            ab_adv(C);  /* skip dot */
            uint32_t mem = ab_node(C, TK_AST_IDENT, C->P->pos);
            if (ab_ctyp(C) == TK_TOK_IDENT) {
                C->P->nodes[mem].d.text.off = ab_cur(C)->off;
                C->P->nodes[mem].d.text.len = ab_cur(C)->len;
                ab_adv(C);
            }
            ab_achld(C, acc, nd);
            ab_achld(C, acc, mem);
            nd = acc;
        }

        /* set index: signal[n] */
        if (ab_isop(C, "lbracket")) {
            uint32_t idx = ab_node(C, TK_AST_INDEX, C->P->pos);
            ab_adv(C);
            ab_achld(C, idx, nd);
            ab_achld(C, idx, ab_expr(C, 1));
            if (ab_isop(C, "rbracket")) ab_adv(C);
            nd = idx;
        }

        return nd;
    }

    /* fallthrough — emit empty node, advance */
    ab_err(C, "expected expression");
    uint32_t nd = ab_node(C, TK_AST_INT_LIT, C->P->pos);
    if (!ab_eof(C)) ab_adv(C);
    return nd;
}

static uint32_t
ab_expr(ab_ctx_t *C, int minp)
{
    uint32_t left = ab_primary(C);

    KA_GUARD(g, 1000);
    while (g--) {
        int p = ab_oprec(C);
        if (p < 0 || p < minp) break;

        uint32_t tok = C->P->pos;
        ab_adv(C);
        /* higher priority binds tighter */
        uint32_t right = ab_expr(C, p);

        uint32_t bin = ab_node(C, TK_AST_BINARY_OP, tok);
        ab_achld(C, bin, left);
        ab_achld(C, bin, right);
        left = bin;
    }

    return left;
}

/* ---- Pin / node declaration ----
 * name PIN [number] [ISTYPE 'attr']; */

static uint32_t
ab_pindecl(ab_ctx_t *C)
{
    uint32_t nd = ab_node(C, TK_AST_PORT, C->P->pos);

    /* signal name */
    if (ab_ctyp(C) == TK_TOK_IDENT) {
        uint32_t name = ab_node(C, TK_AST_IDENT, C->P->pos);
        C->P->nodes[name].d.text.off = ab_cur(C)->off;
        C->P->nodes[name].d.text.len = ab_cur(C)->len;
        ab_achld(C, nd, name);
        ab_adv(C);
    }

    /* PIN or NODE keyword */
    if (ab_iskw(C, C->kw.kw_pin) || ab_iskw(C, C->kw.kw_node))
        ab_adv(C);

    /* optional pin number */
    if (ab_ctyp(C) == TK_TOK_INT_LIT) ab_adv(C);

    /* optional ISTYPE 'attr' */
    if (ab_iskw(C, C->kw.kw_istype)) {
        ab_adv(C);
        if (ab_ctyp(C) == TK_TOK_STR_LIT) ab_adv(C);
    }

    return nd;
}

/* ---- Equation: signal [=|:=|?=|?:=] expr; ---- */

static uint32_t
ab_equation(ab_ctx_t *C)
{
    uint32_t lhs = ab_expr(C, 1);

    /* assignment operator */
    int is_reg = 0;
    if (ab_isop(C, "assign") || ab_isop(C, "dc_assign")) {
        ab_adv(C);
    } else if (ab_isop(C, "reg_assign") || ab_isop(C, "dc_reg")) {
        is_reg = 1;
        ab_adv(C);
    } else {
        /* bare expression — skip to semicolon */
        ab_skipsemi(C);
        return lhs;
    }

    uint32_t rhs = ab_expr(C, 1);
    uint32_t asgn = ab_node(C,
        is_reg ? TK_AST_NONBLOCK : TK_AST_ASSIGN, C->P->pos);
    ab_achld(C, asgn, lhs);
    ab_achld(C, asgn, rhs);

    if (ab_isop(C, "semi")) ab_adv(C);

    return asgn;
}

/* ---- WHEN-THEN-ELSE ---- */

static uint32_t
ab_when(ab_ctx_t *C)
{
    uint32_t nd = ab_node(C, TK_AST_IF, C->P->pos);
    ab_adv(C);  /* skip WHEN */

    ab_achld(C, nd, ab_expr(C, 1));

    if (ab_iskw(C, C->kw.kw_then)) ab_adv(C);

    /* then clause: equation or block */
    if (ab_isop(C, "lbrace")) {
        ab_adv(C);
        uint32_t blk = ab_node(C, TK_AST_BEGIN_END, C->P->pos);
        KA_GUARD(g, 1000);
        while (!ab_eof(C) && !ab_isop(C, "rbrace") && g--)
            ab_achld(C, blk, ab_equation(C));
        if (ab_isop(C, "rbrace")) ab_adv(C);
        ab_achld(C, nd, blk);
    } else {
        ab_achld(C, nd, ab_equation(C));
    }

    /* optional ELSE */
    if (ab_iskw(C, C->kw.kw_else)) {
        ab_adv(C);
        if (ab_iskw(C, C->kw.kw_when))
            ab_achld(C, nd, ab_when(C));
        else
            ab_achld(C, nd, ab_equation(C));
    }

    return nd;
}

/* ---- IF-THEN-ELSE (in state diagram) ---- */

static uint32_t ab_sttrans(ab_ctx_t *C);

static uint32_t
ab_if(ab_ctx_t *C)
{
    uint32_t nd = ab_node(C, TK_AST_IF, C->P->pos);
    ab_adv(C);  /* skip IF */

    ab_achld(C, nd, ab_expr(C, 1));

    if (ab_iskw(C, C->kw.kw_then)) ab_adv(C);

    ab_achld(C, nd, ab_sttrans(C));

    if (ab_iskw(C, C->kw.kw_else))  {
        ab_adv(C);
        ab_achld(C, nd, ab_sttrans(C));
    }

    return nd;
}

/* ---- State transition target ---- */

static uint32_t
ab_sttrans(ab_ctx_t *C)
{
    if (ab_iskw(C, C->kw.kw_if))
        return ab_if(C);

    if (ab_iskw(C, C->kw.kw_goto)) {
        ab_adv(C);
    }

    /* state name */
    uint32_t nd = ab_node(C, TK_AST_IDENT, C->P->pos);
    if (ab_ctyp(C) == TK_TOK_IDENT) {
        C->P->nodes[nd].d.text.off = ab_cur(C)->off;
        C->P->nodes[nd].d.text.len = ab_cur(C)->len;
        ab_adv(C);
    }

    /* optional WITH outputs */
    if (ab_iskw(C, C->kw.kw_with)) {
        ab_adv(C);
        if (ab_isop(C, "lbrace")) {
            ab_adv(C);
            KA_GUARD(g, 100);
            while (!ab_eof(C) && !ab_isop(C, "rbrace") && g--)
                ab_equation(C);
            if (ab_isop(C, "rbrace")) ab_adv(C);
        } else {
            ab_equation(C);
            if (ab_iskw(C, C->kw.kw_endwith)) ab_adv(C);
        }
    }

    if (ab_isop(C, "semi")) ab_adv(C);

    return nd;
}

/* ---- EQUATIONS section ---- */

static void
ab_eqns(ab_ctx_t *C, uint32_t mod)
{
    ab_adv(C);  /* skip EQUATIONS keyword */

    KA_GUARD(g, 10000);
    while (!ab_eof(C) && g--) {
        /* stop at next section keyword or END */
        if (ab_iskw(C, C->kw.kw_eqns) ||
            ab_iskw(C, C->kw.kw_tt) ||
            ab_iskw(C, C->kw.kw_sd) ||
            ab_iskw(C, C->kw.kw_tv) ||
            ab_iskw(C, C->kw.kw_fuses) ||
            ab_iskw(C, C->kw.kw_end))
            break;

        if (ab_iskw(C, C->kw.kw_when)) {
            ab_achld(C, mod, ab_when(C));
        } else {
            ab_achld(C, mod, ab_equation(C));
        }
    }
}

/* ---- TRUTH_TABLE section ---- */

static void
ab_ttbl(ab_ctx_t *C, uint32_t mod)
{
    ab_adv(C);  /* skip TRUTH_TABLE keyword */

    /* header: ([inputs] -> [outputs]); */
    KA_GUARD(g, 10000);
    while (!ab_eof(C) && !ab_isop(C, "semi") && g--)
        ab_adv(C);
    if (ab_isop(C, "semi")) ab_adv(C);

    /* rows: [values] -> [values]; */
    KA_GUARD(g2, 10000);
    while (!ab_eof(C) && g2--) {
        if (ab_iskw(C, C->kw.kw_eqns) ||
            ab_iskw(C, C->kw.kw_tt) ||
            ab_iskw(C, C->kw.kw_sd) ||
            ab_iskw(C, C->kw.kw_tv) ||
            ab_iskw(C, C->kw.kw_fuses) ||
            ab_iskw(C, C->kw.kw_end))
            break;

        /* each row is a case item */
        uint32_t row = ab_node(C, TK_AST_CASE_ITEM, C->P->pos);

        /* input values */
        if (ab_isop(C, "lbracket")) {
            ab_adv(C);
            KA_GUARD(gi, 256);
            while (!ab_eof(C) && !ab_isop(C, "rbracket") && gi--)
                { ab_achld(C, row, ab_expr(C, 1));
                  if (ab_isop(C, "comma")) ab_adv(C); }
            if (ab_isop(C, "rbracket")) ab_adv(C);
        }

        /* -> */
        if (ab_isop(C, "arrow")) ab_adv(C);

        /* output values */
        if (ab_isop(C, "lbracket")) {
            ab_adv(C);
            KA_GUARD(go, 256);
            while (!ab_eof(C) && !ab_isop(C, "rbracket") && go--)
                { ab_achld(C, row, ab_expr(C, 1));
                  if (ab_isop(C, "comma")) ab_adv(C); }
            if (ab_isop(C, "rbracket")) ab_adv(C);
        }

        if (ab_isop(C, "semi")) ab_adv(C);

        ab_achld(C, mod, row);
    }
}

/* ---- STATE_DIAGRAM section ---- */

static void
ab_stdiag(ab_ctx_t *C, uint32_t mod)
{
    ab_adv(C);  /* skip STATE_DIAGRAM keyword */

    /* state register name */
    if (ab_ctyp(C) == TK_TOK_IDENT) ab_adv(C);

    KA_GUARD(g, 10000);
    while (!ab_eof(C) && g--) {
        if (ab_iskw(C, C->kw.kw_eqns) ||
            ab_iskw(C, C->kw.kw_tt) ||
            ab_iskw(C, C->kw.kw_sd) ||
            ab_iskw(C, C->kw.kw_tv) ||
            ab_iskw(C, C->kw.kw_fuses) ||
            ab_iskw(C, C->kw.kw_end))
            break;

        /* STATE name: outputs; transitions; */
        if (ab_iskw(C, C->kw.kw_state)) {
            ab_adv(C);

            uint32_t st = ab_node(C, TK_AST_CASE_ITEM, C->P->pos);

            /* state name */
            if (ab_ctyp(C) == TK_TOK_IDENT) {
                uint32_t sn = ab_node(C, TK_AST_IDENT, C->P->pos);
                C->P->nodes[sn].d.text.off = ab_cur(C)->off;
                C->P->nodes[sn].d.text.len = ab_cur(C)->len;
                ab_achld(C, st, sn);
                ab_adv(C);
            }

            /* colon after state name */
            /* (it's not defined as an op but shows up as :) */
            /* skip any non-structural tokens until IF or GOTO or ; */
            KA_GUARD(gs, 1000);
            while (!ab_eof(C) && gs--) {
                if (ab_iskw(C, C->kw.kw_if)) {
                    ab_achld(C, st, ab_if(C));
                    break;
                }
                if (ab_iskw(C, C->kw.kw_goto)) {
                    ab_achld(C, st, ab_sttrans(C));
                    break;
                }
                if (ab_iskw(C, C->kw.kw_state) ||
                    ab_iskw(C, C->kw.kw_end))
                    break;

                /* output equations within state */
                if (ab_ctyp(C) == TK_TOK_IDENT)  {
                    ab_achld(C, st, ab_equation(C));
                } else {
                    ab_adv(C);
                }
            }

            ab_achld(C, mod, st);
        } else {
            ab_adv(C);  /* skip unknown token */
        }
    }
}

/* ---- DECLARATIONS section ---- */

static void
ab_decls(ab_ctx_t *C, uint32_t mod)
{
    ab_adv(C);  /* skip DECLARATIONS keyword */

    KA_GUARD(g, 10000);
    while (!ab_eof(C) && g--) {
        if (ab_iskw(C, C->kw.kw_eqns) ||
            ab_iskw(C, C->kw.kw_tt) ||
            ab_iskw(C, C->kw.kw_sd) ||
            ab_iskw(C, C->kw.kw_tv) ||
            ab_iskw(C, C->kw.kw_fuses) ||
            ab_iskw(C, C->kw.kw_end))
            break;

        /* constant declaration: name = expr; */
        if (ab_ctyp(C) == TK_TOK_IDENT) {
            /* peek ahead for PIN or NODE keyword */
            uint32_t save = C->P->pos;
            ab_adv(C);

            if (ab_iskw(C, C->kw.kw_pin) ||
                ab_iskw(C, C->kw.kw_node)) {
                C->P->pos = save;
                uint32_t pd = ab_pindecl(C);
                ab_achld(C, mod, pd);
                if (ab_isop(C, "semi")) ab_adv(C);
            } else if (ab_isop(C, "assign")) {
                /* constant: name = value; */
                C->P->pos = save;
                uint32_t cst = ab_node(C, TK_AST_LOCALPARAM, C->P->pos);
                uint32_t nm = ab_node(C, TK_AST_IDENT, C->P->pos);
                C->P->nodes[nm].d.text.off = ab_cur(C)->off;
                C->P->nodes[nm].d.text.len = ab_cur(C)->len;
                ab_achld(C, cst, nm);
                ab_adv(C);  /* name */
                ab_adv(C);  /* = */
                ab_achld(C, cst, ab_expr(C, 1));
                if (ab_isop(C, "semi")) ab_adv(C);
                ab_achld(C, mod, cst);
            } else {
                /* comma-separated pin list: a,b,c PIN; */
                C->P->pos = save;
                KA_GUARD(gp, 256);
                while (!ab_eof(C) && !ab_isop(C, "semi") && gp--) {
                    if (ab_ctyp(C) == TK_TOK_IDENT &&
                        !ab_iskw(C, C->kw.kw_pin) &&
                        !ab_iskw(C, C->kw.kw_node)) {
                        uint32_t pd = ab_pindecl(C);
                        ab_achld(C, mod, pd);
                    } else {
                        ab_adv(C);
                    }
                    if (ab_isop(C, "comma")) ab_adv(C);
                }
                if (ab_isop(C, "semi")) ab_adv(C);
            }
        } else {
            ab_adv(C);
        }
    }
}

/* ---- Top-level MODULE parser ---- */

int
ab_parse(tk_parse_t *P, const tk_lex_t *L)
{
    ab_ctx_t C;
    uint32_t root;

    if (!P || !L) return -1;

    C.P = P;
    C.L = L;

    /* cache keyword IDs */
    C.kw.kw_module  = ab_kwfn(L, "module");
    C.kw.kw_title   = ab_kwfn(L, "title");
    C.kw.kw_decl    = ab_kwfn(L, "declarations");
    C.kw.kw_device  = ab_kwfn(L, "device");
    C.kw.kw_pin     = ab_kwfn(L, "pin");
    C.kw.kw_node    = ab_kwfn(L, "node");
    C.kw.kw_istype  = ab_kwfn(L, "istype");
    C.kw.kw_eqns    = ab_kwfn(L, "equations");
    C.kw.kw_tt      = ab_kwfn(L, "truth_table");
    C.kw.kw_sd      = ab_kwfn(L, "state_diagram");
    C.kw.kw_sr      = ab_kwfn(L, "state_register");
    C.kw.kw_state   = ab_kwfn(L, "state");
    C.kw.kw_tv      = ab_kwfn(L, "test_vectors");
    C.kw.kw_end     = ab_kwfn(L, "end");
    C.kw.kw_if      = ab_kwfn(L, "if");
    C.kw.kw_then    = ab_kwfn(L, "then");
    C.kw.kw_else    = ab_kwfn(L, "else");
    C.kw.kw_case    = ab_kwfn(L, "case");
    C.kw.kw_endcase = ab_kwfn(L, "endcase");
    C.kw.kw_when    = ab_kwfn(L, "when");
    C.kw.kw_with    = ab_kwfn(L, "with");
    C.kw.kw_endwith = ab_kwfn(L, "endwith");
    C.kw.kw_goto    = ab_kwfn(L, "goto");
    C.kw.kw_fuses   = ab_kwfn(L, "fuses");
    C.kw.kw_macro   = ab_kwfn(L, "macro");
    C.kw.kw_ext     = ab_kwfn(L, "external");
    C.kw.kw_intfc   = ab_kwfn(L, "interface");
    C.kw.kw_fblk    = ab_kwfn(L, "functional_block");
    C.kw.kw_lib     = ab_kwfn(L, "library");
    C.kw.kw_trace   = ab_kwfn(L, "trace");
    C.kw.kw_prop    = ab_kwfn(L, "property");
    C.kw.kw_opts    = ab_kwfn(L, "options");
    C.kw.kw_in      = ab_kwfn(L, "in");

    /* root node */
    root = ab_node(&C, TK_AST_ROOT, 0);

    KA_GUARD(gm, 100);
    while (!ab_eof(&C) && gm--) {
        /* MODULE name */
        if (ab_iskw(&C, C.kw.kw_module)) {
            ab_adv(&C);

            uint32_t mod = ab_node(&C, TK_AST_MODULE, C.P->pos);

            /* module name */
            if (ab_ctyp(&C) == TK_TOK_IDENT) {
                uint32_t nm = ab_node(&C, TK_AST_IDENT, C.P->pos);
                C.P->nodes[nm].d.text.off = ab_cur(&C)->off;
                C.P->nodes[nm].d.text.len = ab_cur(&C)->len;
                ab_achld(&C, mod, nm);
                ab_adv(&C);
            }

            if (ab_isop(&C, "semi")) ab_adv(&C);

            /* TITLE */
            if (ab_iskw(&C, C.kw.kw_title)) {
                ab_adv(&C);
                if (ab_ctyp(&C) == TK_TOK_STR_LIT) ab_adv(&C);
            }

            /* device DEVICE 'type'; */
            KA_GUARD(gd, 10);
            while (ab_ctyp(&C) == TK_TOK_IDENT && gd--) {
                ab_adv(&C);  /* device name */
                if (ab_iskw(&C, C.kw.kw_device)) {
                    ab_adv(&C);
                    if (ab_ctyp(&C) == TK_TOK_STR_LIT) ab_adv(&C);
                    if (ab_isop(&C, "semi")) ab_adv(&C);
                } else {
                    break;
                }
            }

            /* sections */
            KA_GUARD(gs, 100);
            while (!ab_eof(&C) && !ab_iskw(&C, C.kw.kw_end) && gs--) {
                if (ab_iskw(&C, C.kw.kw_decl))
                    ab_decls(&C, mod);
                else if (ab_iskw(&C, C.kw.kw_eqns))
                    ab_eqns(&C, mod);
                else if (ab_iskw(&C, C.kw.kw_tt))
                    ab_ttbl(&C, mod);
                else if (ab_iskw(&C, C.kw.kw_sd))
                    ab_stdiag(&C, mod);
                else if (ab_iskw(&C, C.kw.kw_tv)) {
                    /* test vectors — skip for synthesis */
                    ab_adv(&C);
                    KA_GUARD(gt, 10000);
                    while (!ab_eof(&C) && !ab_iskw(&C, C.kw.kw_end) && gt--)
                        ab_adv(&C);
                } else if (ab_iskw(&C, C.kw.kw_fuses)) {
                    /* fuses — skip for synthesis */
                    ab_adv(&C);
                    KA_GUARD(gf, 10000);
                    while (!ab_eof(&C) &&
                           !ab_iskw(&C, C.kw.kw_eqns) &&
                           !ab_iskw(&C, C.kw.kw_end) && gf--)
                        ab_adv(&C);
                } else {
                    ab_adv(&C);
                }
            }

            /* END name */
            if (ab_iskw(&C, C.kw.kw_end)) {
                ab_adv(&C);
                if (ab_ctyp(&C) == TK_TOK_IDENT) ab_adv(&C);
            }

            ab_achld(&C, root, mod);
        } else {
            ab_adv(&C);
        }
    }

    return 0;
}
