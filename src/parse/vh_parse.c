/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * vh_parse.c -- VHDL parser for Takahe
 *
 * Parses the synthesisable subset of IEEE 1076-2008 and maps
 * it to the same AST nodes as the SystemVerilog parser. The
 * lowerer, optimiser, and backend don't care whether the
 * source was VHDL or SV — they see RT_AND, RT_MUX, RT_DFF.
 *
 * VHDL entity+architecture → TK_AST_MODULE
 * VHDL process             → TK_AST_ALWAYS_COMB / TK_AST_ALWAYS_FF
 * VHDL signal <=           → TK_AST_BLOCK_ASSIGN / TK_AST_NONBLOCK
 * VHDL case/when           → TK_AST_CASE / TK_AST_CASE_ITEM
 *
 * The US Department of Defense commissioned VHDL in 1980 because
 * they needed hardware descriptions that were unambiguous,
 * strongly typed, and verbose enough to fill a procurement
 * binder. They got all three. The Sumerians described their
 * hardware with clay — less verbose, equally durable.
 *
 * JPL Power of 10: bounded loops, no alloc, no recursion
 * deeper than VHDL nesting depth.
 */

#include "takahe.h"

/* ---- Intern a string into the lex pool ---- */

static uint32_t
vp_sint(const tk_lex_t *L, const char *s, uint16_t len)
{
    tk_lex_t *ml = (tk_lex_t *)L; /* pool is mutable at parse time */
    uint32_t off;
    if (ml->str_len + len + 1 > ml->str_max) return 0;
    off = ml->str_len;
    memcpy(ml->strs + off, s, len);
    ml->strs[off + len] = '\0';
    ml->str_len += len + 1;
    return off;
}

/* ---- Keyword ID Lookup ---- */

static uint16_t
vk_find(const tk_lex_t *L, const char *name)
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

/* ---- Token Stream Helpers ----
 * Same pattern as SV parser. Different language, same ritual. */

static const tk_token_t *
vp_cur(const tk_parse_t *P)
{
    if (P->pos >= P->n_tok) return &P->tokens[P->n_tok - 1];
    return &P->tokens[P->pos];
}

static tk_toktype_t vp_ctyp(const tk_parse_t *P) { return vp_cur(P)->type; }
static uint16_t     vp_csub(const tk_parse_t *P) { return vp_cur(P)->sub; }

static void vp_adv(tk_parse_t *P)
{
    if (P->pos < P->n_tok) P->pos++;
}

static int vp_iskw(const tk_parse_t *P, uint16_t kwid)
{
    if (kwid == TK_KW_NONE) return 0;
    return vp_cur(P)->type == TK_TOK_KWD && vp_cur(P)->sub == kwid;
}

static int vp_isop(const tk_parse_t *P, const char *chars)
{
    const tk_token_t *t = vp_cur(P);
    if (t->type != TK_TOK_OP) return 0;
    return strcmp(P->lex->strs + P->lex->ops[t->sub].chars_off, chars) == 0;
}

static int vp_mkw(tk_parse_t *P, uint16_t kwid)
{
    if (vp_iskw(P, kwid)) { vp_adv(P); return 1; }
    return 0;
}

static int vp_mop(tk_parse_t *P, const char *c)
{
    if (vp_isop(P, c)) { vp_adv(P); return 1; }
    return 0;
}

static void vp_ekw(tk_parse_t *P, uint16_t kwid)
{
    if (!vp_mkw(P, kwid) && P->n_err < TK_MAX_ERRORS) {
        const tk_token_t *t = vp_cur(P);
        const char *exp = (kwid != TK_KW_NONE)
            ? P->lex->strs + P->lex->kwds[kwid].name_off : "?";
        tk_err_t *e = &P->errors[P->n_err++];
        e->line = t->line; e->col = t->col;
        snprintf(e->msg, sizeof(e->msg), "expected '%s', got '%.*s'",
                 exp, t->len > 30 ? 30 : t->len, P->lex->strs + t->off);
    }
}

static void vp_eop(tk_parse_t *P, const char *c)
{
    if (!vp_mop(P, c) && P->n_err < TK_MAX_ERRORS) {
        const tk_token_t *t = vp_cur(P);
        tk_err_t *e = &P->errors[P->n_err++];
        e->line = t->line; e->col = t->col;
        snprintf(e->msg, sizeof(e->msg), "expected '%s', got '%.*s'",
                 c, t->len > 30 ? 30 : t->len, P->lex->strs + t->off);
    }
}

/* ---- AST Node Management ---- */

static uint32_t
vp_alloc(tk_parse_t *P, tk_ast_type_t type)
{
    uint32_t idx;
    if (P->n_node >= P->max_node) return 0;
    idx = P->n_node++;
    memset(&P->nodes[idx], 0, sizeof(tk_node_t));
    P->nodes[idx].type = type;
    P->nodes[idx].line = vp_cur(P)->line;
    P->nodes[idx].col  = vp_cur(P)->col;
    return idx;
}

static void
vp_achld(tk_parse_t *P, uint32_t par, uint32_t ch)
{
    if (par == 0 || ch == 0) return;
    if (KA_CHK(par, P->n_node)) return;
    if (KA_CHK(ch, P->n_node)) return;
    P->nodes[ch].next_sib = 0;
    if (P->nodes[par].first_child == 0) {
        P->nodes[par].first_child = ch;
    } else {
        P->nodes[P->nodes[par].last_child].next_sib = ch;
    }
    P->nodes[par].last_child = ch;
}

/* ---- Error Recovery ---- */

static void
vp_sync(tk_parse_t *P)
{
    KA_GUARD(g, 10000);
    while (vp_ctyp(P) != TK_TOK_EOF && g--) {
        if (vp_isop(P, ";")) { vp_adv(P); return; }
        if (vp_iskw(P, P->kw.vh_end)) return;
        vp_adv(P);
    }
}

/* ---- Forward Declarations ---- */

static uint32_t vp_expr(tk_parse_t *P);
static void     vp_seq(tk_parse_t *P, uint32_t par, int is_reg);

/* ---- Expression Parser ----
 * VHDL expressions: precedence climbing.
 * Logical: and or nand nor xor xnor (all same precedence, left-assoc)
 * Relational: = /= < > <= >=
 * Additive: + - &
 * Multiplicative: * / mod rem
 * Unary: not abs
 * Primary: ident, literal, (expr), function call */

static uint32_t vp_prim(tk_parse_t *P);

static uint32_t
vp_prim(tk_parse_t *P)
{
    const tk_token_t *t = vp_cur(P);

    /* Parenthesised or aggregate */
    if (vp_isop(P, "(")) {
        uint32_t inner;
        vp_adv(P);
        /* Check for aggregate: (others => ...) */
        if (vp_iskw(P, P->kw.vh_others)) {
            uint32_t cat = vp_alloc(P, TK_AST_CONCAT);
            vp_adv(P); /* others */
            vp_eop(P, "=>");
            inner = vp_expr(P);
            vp_achld(P, cat, inner);
            vp_eop(P, ")");
            return cat;
        }
        inner = vp_expr(P);
        vp_eop(P, ")");
        return inner;
    }

    /* Unary not */
    if (vp_iskw(P, P->kw.vh_not)) {
        uint32_t un = vp_alloc(P, TK_AST_UNARY_OP);
        P->nodes[un].op = vp_csub(P); /* store operator */
        P->nodes[un].d.text.off = vp_cur(P)->off;
        P->nodes[un].d.text.len = vp_cur(P)->len;
        vp_adv(P);
        vp_achld(P, un, vp_prim(P));
        return un;
    }

    /* Identifier (possibly followed by ( ) for function call or type cast) */
    if (vp_ctyp(P) == TK_TOK_IDENT) {
        uint32_t id = vp_alloc(P, TK_AST_IDENT);
        P->nodes[id].d.text.off = t->off;
        P->nodes[id].d.text.len = t->len;
        vp_adv(P);

        /* Function call: ident(args) — like unsigned(a) */
        if (vp_isop(P, "(")) {
            uint32_t call = vp_alloc(P, TK_AST_CALL);
            P->nodes[call].d.text.off = P->nodes[id].d.text.off;
            P->nodes[call].d.text.len = P->nodes[id].d.text.len;
            vp_adv(P); /* ( */
            if (!vp_isop(P, ")")) {
                vp_achld(P, call, vp_expr(P));
                KA_GUARD(ga, 64);
                while (vp_isop(P, ",") && ga--) {
                    vp_adv(P);
                    vp_achld(P, call, vp_expr(P));
                }
            }
            vp_eop(P, ")");
            return call;
        }

        return id;
    }

    /* Integer literal */
    if (vp_ctyp(P) == TK_TOK_INT_LIT) {
        uint32_t lit = vp_alloc(P, TK_AST_INT_LIT);
        P->nodes[lit].d.text.off = t->off;
        P->nodes[lit].d.text.len = t->len;
        vp_adv(P);
        return lit;
    }

    /* String literal — VHDL "001" means 3-bit binary 001.
     * Convert to SV-style N'bXXX so the constant evaluator
     * and lowerer understand it. Cuneiform to hieroglyphics. */
    if (vp_ctyp(P) == TK_TOK_STR_LIT) {
        const char *s = P->lex->strs + t->off;
        uint16_t sl = t->len;

        /* Check if it's a binary string: "0101..." */
        if (sl >= 3 && s[0] == '"' && s[sl-1] == '"') {
            uint16_t nbits = sl - 2;  /* strip quotes */
            int is_bin = 1;
            uint16_t k;
            for (k = 1; k < sl - 1; k++) {
                if (s[k] != '0' && s[k] != '1') { is_bin = 0; break; }
            }
            if (is_bin && nbits > 0 && nbits < 32) {
                /* Intern as N'bXXX */
                char buf[48];
                int blen = snprintf(buf, sizeof(buf), "%u'b%.*s",
                                    (unsigned)nbits, (int)nbits, s + 1);
                if (blen > 0 && blen < 48) {
                    uint32_t off = vp_sint(P->lex, buf, (uint16_t)blen);
                    uint32_t lit = vp_alloc(P, TK_AST_INT_LIT);
                    P->nodes[lit].d.text.off = off;
                    P->nodes[lit].d.text.len = (uint16_t)blen;
                    vp_adv(P);
                    return lit;
                }
            }
        }

        /* Not a binary string — emit as string literal */
        {
            uint32_t lit = vp_alloc(P, TK_AST_STR_LIT);
            P->nodes[lit].d.text.off = t->off;
            P->nodes[lit].d.text.len = t->len;
            vp_adv(P);
            return lit;
        }
    }

    /* Tick literal '0' '1' emitted as tick int tick */
    if (vp_isop(P, "'")) {
        vp_adv(P); /* ' */
        if (vp_ctyp(P) == TK_TOK_INT_LIT) {
            uint32_t lit = vp_alloc(P, TK_AST_INT_LIT);
            P->nodes[lit].d.text.off = vp_cur(P)->off;
            P->nodes[lit].d.text.len = vp_cur(P)->len;
            vp_adv(P);
            vp_mop(P, "'"); /* closing tick */
            return lit;
        }
        /* Attribute tick — skip for now */
        return 0;
    }

    /* Keyword operators used as function names: and, or, etc.
     * These can appear in expressions like "a and b". Handle below. */
    return 0;
}

/* ---- Binary operator precedence ----
 * Returns precedence (0 = not an operator), advances if matched. */

static int
vp_binp(const tk_parse_t *P)
{
    /* Multiplicative: * / mod rem — precedence 4 */
    if (vp_isop(P, "*") || vp_isop(P, "/")) return 4;

    /* Additive: + - & — precedence 3 */
    if (vp_isop(P, "+") || vp_isop(P, "-") || vp_isop(P, "&")) return 3;

    /* Relational: = /= < > <= >= — precedence 2 */
    if (vp_isop(P, "=") || vp_isop(P, "/=") ||
        vp_isop(P, "<") || vp_isop(P, ">") ||
        vp_isop(P, ">=")) return 2;
    /* <= is ambiguous: signal assign or less-or-equal.
     * In expression context it's comparison. */
    /* We skip <= here — handled at statement level */

    /* Logical: and or xor nand nor xnor — precedence 1 */
    if (vp_iskw(P, P->kw.vh_and) || vp_iskw(P, P->kw.vh_or) ||
        vp_iskw(P, P->kw.vh_xor) || vp_iskw(P, P->kw.vh_nand) ||
        vp_iskw(P, P->kw.vh_nor) || vp_iskw(P, P->kw.vh_xnor))
        return 1;

    return 0;
}

static uint32_t
vp_expr(tk_parse_t *P)
{
    uint32_t left = vp_prim(P);
    if (left == 0) return 0;

    KA_GUARD(g, 256);
    while (vp_binp(P) > 0 && g--) {
        uint32_t bop = vp_alloc(P, TK_AST_BINARY_OP);
        const tk_token_t *opt = vp_cur(P);

        /* Store operator text */
        P->nodes[bop].d.text.off = opt->off;
        P->nodes[bop].d.text.len = opt->len;

        /* Map VHDL keyword operators to op index */
        if (opt->type == TK_TOK_KWD) {
            /* Store the keyword text as operator name */
            P->nodes[bop].op = opt->sub;
        } else {
            P->nodes[bop].op = opt->sub;
        }

        vp_adv(P);
        vp_achld(P, bop, left);
        vp_achld(P, bop, vp_prim(P));
        left = bop;
    }

    return left;
}

/* ---- Parse port list in entity ----
 * port ( name : direction type ; ... ) */

static void
vp_ports(tk_parse_t *P, uint32_t parent)
{
    vp_eop(P, "(");

    KA_GUARD(g, 256);
    while (!vp_isop(P, ")") && vp_ctyp(P) != TK_TOK_EOF && g--) {
        uint32_t port = vp_alloc(P, TK_AST_PORT);
        uint32_t id;

        /* Port name */
        if (vp_ctyp(P) != TK_TOK_IDENT) { vp_sync(P); continue; }
        id = vp_alloc(P, TK_AST_IDENT);
        P->nodes[id].d.text.off = vp_cur(P)->off;
        P->nodes[id].d.text.len = vp_cur(P)->len;
        vp_adv(P);

        vp_eop(P, ":");

        /* Direction: intern SV-compatible strings so the lowerer
         * sees "input"/"output"/"inout" regardless of HDL.
         * VHDL says "in", the lowerer expects "input".
         * Like translating between French procurement forms
         * and American ones — same information, different words. */
        if (vp_iskw(P, P->kw.vh_in)) {
            P->nodes[port].d.text.off = vp_sint(P->lex, "input", 5);
            P->nodes[port].d.text.len = 5;
            vp_adv(P);
        } else if (vp_iskw(P, P->kw.vh_out)) {
            P->nodes[port].d.text.off = vp_sint(P->lex, "output", 6);
            P->nodes[port].d.text.len = 6;
            vp_adv(P);
        } else if (vp_iskw(P, P->kw.vh_inout)) {
            P->nodes[port].d.text.off = vp_sint(P->lex, "inout", 5);
            P->nodes[port].d.text.len = 5;
            vp_adv(P);
        } else if (vp_iskw(P, P->kw.vh_buffer)) {
            P->nodes[port].d.text.off = vp_sint(P->lex, "output", 6);
            P->nodes[port].d.text.len = 6;
            vp_adv(P);
        }

        /* Type: std_logic, std_logic_vector(N downto 0), unsigned(...) */
        {
            uint32_t typen = vp_alloc(P, TK_AST_TYPE_SPEC);
            if (vp_ctyp(P) == TK_TOK_IDENT) {
                P->nodes[typen].d.text.off = vp_cur(P)->off;
                P->nodes[typen].d.text.len = vp_cur(P)->len;
                vp_adv(P);
            }
            /* Range: (N downto 0) */
            if (vp_isop(P, "(")) {
                uint32_t rng = vp_alloc(P, TK_AST_RANGE);
                vp_adv(P);
                vp_achld(P, rng, vp_expr(P));
                if (!vp_mkw(P, P->kw.vh_downto)) vp_mkw(P, P->kw.vh_to);
                vp_achld(P, rng, vp_expr(P));
                vp_eop(P, ")");
                vp_achld(P, typen, rng);
            }
            vp_achld(P, port, typen);
        }

        vp_achld(P, port, id);
        vp_achld(P, parent, port);

        vp_mop(P, ";"); /* optional between ports, required before ) */
    }

    vp_eop(P, ")");
    vp_mop(P, ";");
}

/* ---- Parse generic list ----
 * generic ( name : type := default ; ... ) */

static void
vp_gens(tk_parse_t *P, uint32_t parent)
{
    vp_eop(P, "(");

    KA_GUARD(g, 64);
    while (!vp_isop(P, ")") && vp_ctyp(P) != TK_TOK_EOF && g--) {
        uint32_t par = vp_alloc(P, TK_AST_PARAM);

        if (vp_ctyp(P) == TK_TOK_IDENT) {
            P->nodes[par].d.text.off = vp_cur(P)->off;
            P->nodes[par].d.text.len = vp_cur(P)->len;
            vp_adv(P);
        }
        vp_eop(P, ":");
        /* Skip type */
        KA_GUARD(g2, 100);
        while (!vp_isop(P, ":=") && !vp_isop(P, ";") &&
               !vp_isop(P, ")") && vp_ctyp(P) != TK_TOK_EOF && g2--) {
            vp_adv(P);
        }
        /* Default value */
        if (vp_mop(P, ":=")) {
            vp_achld(P, par, vp_expr(P));
        }
        vp_achld(P, parent, par);
        vp_mop(P, ";");
    }

    vp_eop(P, ")");
    vp_mop(P, ";");
}

/* ---- Parse signal/variable declaration ----
 * signal name [, name] : type ; */

static void
vp_sdecl(tk_parse_t *P, uint32_t parent, int is_var)
{
    uint32_t decl = vp_alloc(P, is_var ? TK_AST_VAR_DECL : TK_AST_NET_DECL);
    vp_adv(P); /* signal/variable keyword */

    /* Collect names */
    KA_GUARD(g, 32);
    while (vp_ctyp(P) == TK_TOK_IDENT && g--) {
        uint32_t id = vp_alloc(P, TK_AST_IDENT);
        P->nodes[id].d.text.off = vp_cur(P)->off;
        P->nodes[id].d.text.len = vp_cur(P)->len;
        vp_adv(P);
        vp_achld(P, decl, id);
        if (!vp_mop(P, ",")) break;
    }

    vp_eop(P, ":");

    /* Type with optional range */
    {
        uint32_t typen = vp_alloc(P, TK_AST_TYPE_SPEC);
        if (vp_ctyp(P) == TK_TOK_IDENT) {
            P->nodes[typen].d.text.off = vp_cur(P)->off;
            P->nodes[typen].d.text.len = vp_cur(P)->len;
            vp_adv(P);
        }
        if (vp_isop(P, "(")) {
            uint32_t rng = vp_alloc(P, TK_AST_RANGE);
            vp_adv(P);
            vp_achld(P, rng, vp_expr(P));
            if (!vp_mkw(P, P->kw.vh_downto)) vp_mkw(P, P->kw.vh_to);
            vp_achld(P, rng, vp_expr(P));
            vp_eop(P, ")");
            vp_achld(P, typen, rng);
        }
        /* Prepend type to decl */
        P->nodes[typen].next_sib = P->nodes[decl].first_child;
        P->nodes[decl].first_child = typen;
    }

    vp_eop(P, ";");
    vp_achld(P, parent, decl);
}

/* ---- Parse sequential statements (inside process) ----
 * if/elsif/else, case/when, signal assign <=, variable := */

static void
vp_ifst(tk_parse_t *P, uint32_t parent, int is_reg)
{
    uint32_t ifn = vp_alloc(P, TK_AST_IF);
    vp_adv(P); /* if */

    /* Condition */
    vp_achld(P, ifn, vp_expr(P));
    vp_ekw(P, P->kw.vh_then);

    /* Then body */
    {
        uint32_t blk = vp_alloc(P, TK_AST_BEGIN_END);
        vp_seq(P, blk, is_reg);
        vp_achld(P, ifn, blk);
    }

    /* Elsif chain */
    KA_GUARD(g, 64);
    while (vp_iskw(P, P->kw.vh_elsif) && g--) {
        uint32_t eif = vp_alloc(P, TK_AST_IF);
        vp_adv(P); /* elsif */
        vp_achld(P, eif, vp_expr(P));
        vp_ekw(P, P->kw.vh_then);
        {
            uint32_t blk = vp_alloc(P, TK_AST_BEGIN_END);
            vp_seq(P, blk, is_reg);
            vp_achld(P, eif, blk);
        }
        vp_achld(P, ifn, eif);
    }

    /* Else */
    if (vp_mkw(P, P->kw.vh_else)) {
        uint32_t blk = vp_alloc(P, TK_AST_BEGIN_END);
        vp_seq(P, blk, is_reg);
        vp_achld(P, ifn, blk);
    }

    vp_ekw(P, P->kw.vh_end);
    vp_ekw(P, P->kw.vh_if);
    vp_eop(P, ";");

    vp_achld(P, parent, ifn);
}

static void
vp_case(tk_parse_t *P, uint32_t parent, int is_reg)
{
    uint32_t csn = vp_alloc(P, TK_AST_CASE);
    vp_adv(P); /* case */

    /* Selector expression */
    vp_achld(P, csn, vp_expr(P));
    vp_ekw(P, P->kw.vh_is);

    /* Case items: when value => stmts; */
    KA_GUARD(g, 256);
    while (!vp_iskw(P, P->kw.vh_end) && vp_ctyp(P) != TK_TOK_EOF && g--) {
        if (vp_mkw(P, P->kw.vh_when)) {
            uint32_t item = vp_alloc(P, TK_AST_CASE_ITEM);

            /* Value(s) or "others" */
            if (vp_iskw(P, P->kw.vh_others)) {
                /* Default: no value node, just body */
                vp_adv(P);
            } else {
                vp_achld(P, item, vp_expr(P));
            }

            vp_eop(P, "=>");

            /* Body statements (until next when or end) */
            {
                uint32_t blk = vp_alloc(P, TK_AST_BEGIN_END);
                KA_GUARD(g2, 1000);
                while (!vp_iskw(P, P->kw.vh_when) &&
                       !vp_iskw(P, P->kw.vh_end) &&
                       vp_ctyp(P) != TK_TOK_EOF && g2--) {
                    /* Parse one sequential statement */
                    if (vp_iskw(P, P->kw.vh_if)) {
                        vp_ifst(P, blk, is_reg);
                    } else if (vp_ctyp(P) == TK_TOK_IDENT) {
                        /* Signal/variable assignment */
                        uint32_t asgn;
                        uint32_t lhs = vp_alloc(P, TK_AST_IDENT);
                        P->nodes[lhs].d.text.off = vp_cur(P)->off;
                        P->nodes[lhs].d.text.len = vp_cur(P)->len;
                        vp_adv(P);
                        if (vp_mop(P, "<=")) {
                            asgn = vp_alloc(P,
                                is_reg ? TK_AST_NONBLOCK : TK_AST_BLOCK_ASSIGN);
                            vp_achld(P, asgn, lhs);
                            vp_achld(P, asgn, vp_expr(P));
                            vp_eop(P, ";");
                            vp_achld(P, blk, asgn);
                        } else if (vp_mop(P, ":=")) {
                            asgn = vp_alloc(P, TK_AST_BLOCK_ASSIGN);
                            vp_achld(P, asgn, lhs);
                            vp_achld(P, asgn, vp_expr(P));
                            vp_eop(P, ";");
                            vp_achld(P, blk, asgn);
                        } else {
                            vp_sync(P);
                        }
                    } else if (vp_iskw(P, P->kw.vh_null)) {
                        vp_adv(P); vp_eop(P, ";");
                    } else {
                        vp_sync(P);
                    }
                }
                vp_achld(P, item, blk);
            }
            vp_achld(P, csn, item);
        } else {
            vp_adv(P); /* skip unknown */
        }
    }

    vp_ekw(P, P->kw.vh_end);
    vp_ekw(P, P->kw.vh_case);
    vp_eop(P, ";");

    vp_achld(P, parent, csn);
}

/* ---- Sequential statement list ----
 * Inside process, if body, case body, etc.
 * Stops at: end, elsif, else, when, EOF */

static void
vp_seq(tk_parse_t *P, uint32_t parent, int is_reg)
{
    KA_GUARD(g, 10000);
    while (vp_ctyp(P) != TK_TOK_EOF && g--) {
        /* Stop tokens */
        if (vp_iskw(P, P->kw.vh_end) || vp_iskw(P, P->kw.vh_elsif) ||
            vp_iskw(P, P->kw.vh_else) || vp_iskw(P, P->kw.vh_when))
            return;

        if (vp_iskw(P, P->kw.vh_if)) {
            vp_ifst(P, parent, is_reg);
        } else if (vp_iskw(P, P->kw.vh_case)) {
            vp_case(P, parent, is_reg);
        } else if (vp_iskw(P, P->kw.vh_null)) {
            vp_adv(P); vp_eop(P, ";");
        } else if (vp_iskw(P, P->kw.vh_for)) {
            /* for loop: for i in range loop ... end loop; */
            /* TODO: implement for synthesis */
            vp_sync(P);
        } else if (vp_ctyp(P) == TK_TOK_IDENT) {
            /* Signal/variable assignment */
            uint32_t asgn;
            uint32_t lhs = vp_alloc(P, TK_AST_IDENT);
            P->nodes[lhs].d.text.off = vp_cur(P)->off;
            P->nodes[lhs].d.text.len = vp_cur(P)->len;
            vp_adv(P);

            if (vp_mop(P, "<=")) {
                asgn = vp_alloc(P,
                    is_reg ? TK_AST_NONBLOCK : TK_AST_BLOCK_ASSIGN);
                vp_achld(P, asgn, lhs);
                vp_achld(P, asgn, vp_expr(P));
                vp_eop(P, ";");
                vp_achld(P, parent, asgn);
            } else if (vp_mop(P, ":=")) {
                asgn = vp_alloc(P, TK_AST_BLOCK_ASSIGN);
                vp_achld(P, asgn, lhs);
                vp_achld(P, asgn, vp_expr(P));
                vp_eop(P, ";");
                vp_achld(P, parent, asgn);
            } else {
                vp_sync(P);
            }
        } else {
            vp_adv(P);
        }
    }
}

/* ---- Parse process statement ----
 * process(sensitivity_list) begin ... end process; */

static void
vp_proc(tk_parse_t *P, uint32_t parent)
{
    int is_reg = 0;
    uint32_t proc;

    vp_adv(P); /* process */

    /* Sensitivity list — determines comb vs ff */
    if (vp_isop(P, "(")) {
        vp_adv(P);
        /* Check for rising_edge/falling_edge → sequential */
        /* For now: any sensitivity with clk → always_ff,
         * otherwise → always_comb */
        KA_GUARD(g, 64);
        while (!vp_isop(P, ")") && vp_ctyp(P) != TK_TOK_EOF && g--) {
            if (vp_ctyp(P) == TK_TOK_IDENT) {
                const char *nm = P->lex->strs + vp_cur(P)->off;
                if (vp_cur(P)->len == 3 &&
                    (memcmp(nm, "clk", 3) == 0 ||
                     memcmp(nm, "CLK", 3) == 0))
                    is_reg = 1;
            }
            vp_adv(P);
        }
        vp_mop(P, ")");
    }

    proc = vp_alloc(P, is_reg ? TK_AST_ALWAYS_FF : TK_AST_ALWAYS_COMB);

    /* Sensitivity list as AST node (for DFF inference) */
    if (is_reg) {
        uint32_t sl = vp_alloc(P, TK_AST_SENS_LIST);
        uint32_t se = vp_alloc(P, TK_AST_SENS_EDGE);
        /* Hardcode posedge clk for now — proper detection in
         * the rising_edge() call inside the if body */
        /* Placeholder: proper posedge detection happens when
         * the lowerer sees rising_edge() in the if body */
        P->nodes[se].d.text.off = 0;
        P->nodes[se].d.text.len = 7;
        vp_achld(P, sl, se);
        vp_achld(P, proc, sl);
    }

    vp_mkw(P, P->kw.vh_is); /* optional 'is' */
    vp_ekw(P, P->kw.vh_begin);

    /* Process body */
    {
        uint32_t blk = vp_alloc(P, TK_AST_BEGIN_END);
        vp_seq(P, blk, is_reg);
        vp_achld(P, proc, blk);
    }

    vp_ekw(P, P->kw.vh_end);
    vp_mkw(P, P->kw.vh_process); /* optional */
    vp_mop(P, ";");

    vp_achld(P, parent, proc);
}

/* ---- Parse concurrent signal assignment ----
 * name <= expr; or name <= expr when cond else expr; */

static void
vp_csig(tk_parse_t *P, uint32_t parent)
{
    uint32_t asgn = vp_alloc(P, TK_AST_ASSIGN);
    uint32_t lhs = vp_alloc(P, TK_AST_IDENT);

    P->nodes[lhs].d.text.off = vp_cur(P)->off;
    P->nodes[lhs].d.text.len = vp_cur(P)->len;
    vp_adv(P); /* ident */
    vp_eop(P, "<=");

    vp_achld(P, asgn, lhs);

    /* RHS: expr, or conditional (expr when cond else expr) */
    {
        uint32_t rhs = vp_expr(P);
        if (vp_iskw(P, P->kw.vh_when)) {
            /* Conditional: val when cond else val2
             * Map to ternary: cond ? val : val2 */
            vp_adv(P); /* when */
            {
                uint32_t cond = vp_expr(P);
                uint32_t tern = vp_alloc(P, TK_AST_TERNARY);
                vp_achld(P, tern, cond);
                vp_achld(P, tern, rhs); /* then-value */
                vp_ekw(P, P->kw.vh_else);
                vp_achld(P, tern, vp_expr(P)); /* else-value */
                rhs = tern;
            }
        }
        vp_achld(P, asgn, rhs);
    }

    vp_eop(P, ";");
    vp_achld(P, parent, asgn);
}

/* ---- Parse architecture body ---- */

static void
vp_arch(tk_parse_t *P, uint32_t mod)
{
    vp_adv(P); /* architecture */

    /* Architecture name */
    if (vp_ctyp(P) == TK_TOK_IDENT) vp_adv(P); /* rtl/behavioral/etc */
    vp_ekw(P, P->kw.vh_of);
    if (vp_ctyp(P) == TK_TOK_IDENT) vp_adv(P); /* entity name */
    vp_ekw(P, P->kw.vh_is);

    /* Declarative region: signals, constants, components */
    KA_GUARD(g, 1000);
    while (!vp_iskw(P, P->kw.vh_begin) &&
           vp_ctyp(P) != TK_TOK_EOF && g--) {
        if (vp_iskw(P, P->kw.vh_signal)) {
            vp_sdecl(P, mod, 0);
        } else if (vp_iskw(P, P->kw.vh_variable)) {
            vp_sdecl(P, mod, 1);
        } else if (vp_iskw(P, P->kw.vh_constant)) {
            /* Skip constant decl for now */
            KA_GUARD(g2, 100);
            while (!vp_isop(P, ";") && vp_ctyp(P) != TK_TOK_EOF && g2--)
                vp_adv(P);
            vp_mop(P, ";");
        } else if (vp_iskw(P, P->kw.vh_component)) {
            /* Skip component declarations */
            KA_GUARD(g2, 1000);
            while (vp_ctyp(P) != TK_TOK_EOF && g2--) {
                if (vp_iskw(P, P->kw.vh_end)) {
                    vp_adv(P);
                    vp_mkw(P, P->kw.vh_component);
                    if (vp_ctyp(P) == TK_TOK_IDENT) vp_adv(P);
                    vp_mop(P, ";");
                    break;
                }
                vp_adv(P);
            }
        } else {
            vp_adv(P);
        }
    }

    vp_ekw(P, P->kw.vh_begin);

    /* Concurrent statement region */
    KA_GUARD(g3, 10000);
    while (!vp_iskw(P, P->kw.vh_end) && vp_ctyp(P) != TK_TOK_EOF && g3--) {
        /* Process */
        if (vp_iskw(P, P->kw.vh_process)) {
            vp_proc(P, mod);
            continue;
        }

        /* Label: ident : process/for/... */
        if (vp_ctyp(P) == TK_TOK_IDENT) {
            /* Peek ahead for : (label) or <= (concurrent assign) */
            uint32_t save = P->pos;
            vp_adv(P);

            if (vp_isop(P, "<=")) {
                /* Concurrent signal assignment */
                P->pos = save;
                vp_csig(P, mod);
                continue;
            }

            if (vp_isop(P, ":")) {
                /* Label — skip it, parse what follows */
                vp_adv(P); /* : */
                if (vp_iskw(P, P->kw.vh_process)) {
                    vp_proc(P, mod);
                } else if (vp_iskw(P, P->kw.vh_for)) {
                    /* Generate for — TODO */
                    vp_sync(P);
                } else {
                    /* Component instantiation or other */
                    vp_sync(P);
                }
                continue;
            }

            /* Unknown — restore and skip */
            P->pos = save;
            vp_adv(P);
            continue;
        }

        vp_adv(P);
    }

    vp_ekw(P, P->kw.vh_end);
    vp_mkw(P, P->kw.architecture); /* optional */
    if (vp_ctyp(P) == TK_TOK_IDENT) vp_adv(P); /* optional arch name */
    vp_eop(P, ";");
}

/* ---- Parse entity + architecture pair → MODULE ---- */

static uint32_t
vp_ent(tk_parse_t *P)
{
    uint32_t mod = vp_alloc(P, TK_AST_MODULE);
    vp_adv(P); /* entity */

    /* Entity name */
    if (vp_ctyp(P) == TK_TOK_IDENT) {
        P->nodes[mod].d.text.off = vp_cur(P)->off;
        P->nodes[mod].d.text.len = vp_cur(P)->len;
        vp_adv(P);
    }

    vp_ekw(P, P->kw.vh_is);

    /* Generics */
    if (vp_iskw(P, P->kw.vh_generic)) {
        vp_adv(P);
        vp_gens(P, mod);
    }

    /* Ports */
    if (vp_iskw(P, P->kw.vh_port)) {
        vp_adv(P);
        vp_ports(P, mod);
    }

    /* End entity */
    vp_ekw(P, P->kw.vh_end);
    vp_mkw(P, P->kw.entity); /* optional */
    if (vp_ctyp(P) == TK_TOK_IDENT) vp_adv(P); /* optional name */
    vp_eop(P, ";");

    return mod;
}

/* ---- Public API ---- */

int
vh_pinit(tk_parse_t *P, const tk_lex_t *L)
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

    /* Pre-compute VHDL keyword IDs */
    P->kw.entity        = vk_find(L, "entity");
    P->kw.architecture  = vk_find(L, "architecture");
    P->kw.vh_of         = vk_find(L, "of");
    P->kw.vh_is         = vk_find(L, "is");
    P->kw.vh_begin      = vk_find(L, "begin");
    P->kw.vh_end        = vk_find(L, "end");
    P->kw.vh_port       = vk_find(L, "port");
    P->kw.vh_generic    = vk_find(L, "generic");
    P->kw.vh_signal     = vk_find(L, "signal");
    P->kw.vh_variable   = vk_find(L, "variable");
    P->kw.vh_constant   = vk_find(L, "constant");
    P->kw.vh_process    = vk_find(L, "process");
    P->kw.vh_if         = vk_find(L, "if");
    P->kw.vh_then       = vk_find(L, "then");
    P->kw.vh_elsif      = vk_find(L, "elsif");
    P->kw.vh_else       = vk_find(L, "else");
    P->kw.vh_case       = vk_find(L, "case");
    P->kw.vh_when       = vk_find(L, "when");
    P->kw.vh_others     = vk_find(L, "others");
    P->kw.vh_in         = vk_find(L, "in");
    P->kw.vh_out        = vk_find(L, "out");
    P->kw.vh_inout      = vk_find(L, "inout");
    P->kw.vh_buffer     = vk_find(L, "buffer");
    P->kw.vh_downto     = vk_find(L, "downto");
    P->kw.vh_to         = vk_find(L, "to");
    P->kw.vh_for        = vk_find(L, "for");
    P->kw.vh_generate   = vk_find(L, "generate");
    P->kw.vh_loop       = vk_find(L, "loop");
    P->kw.vh_while      = vk_find(L, "while");
    P->kw.vh_exit       = vk_find(L, "exit");
    P->kw.vh_next       = vk_find(L, "next");
    P->kw.vh_return     = vk_find(L, "return");
    P->kw.vh_not        = vk_find(L, "not");
    P->kw.vh_and        = vk_find(L, "and");
    P->kw.vh_or         = vk_find(L, "or");
    P->kw.vh_xor        = vk_find(L, "xor");
    P->kw.vh_nand       = vk_find(L, "nand");
    P->kw.vh_nor        = vk_find(L, "nor");
    P->kw.vh_xnor       = vk_find(L, "xnor");
    P->kw.vh_library    = vk_find(L, "library");
    P->kw.vh_use        = vk_find(L, "use");
    P->kw.vh_all        = vk_find(L, "all");
    P->kw.vh_component  = vk_find(L, "component");
    P->kw.vh_null       = vk_find(L, "null");
    P->kw.vh_open       = vk_find(L, "open");
    P->kw.vh_map        = vk_find(L, "map");
    P->kw.vh_select     = vk_find(L, "select");
    P->kw.vh_with       = vk_find(L, "with");

    return 0;
}

int
vh_parse(tk_parse_t *P)
{
    uint32_t root = vp_alloc(P, TK_AST_ROOT);
    uint32_t last_mod = 0;

    KA_GUARD(g, 10000);
    while (vp_ctyp(P) != TK_TOK_EOF && g--) {
        /* library / use — skip */
        if (vp_iskw(P, P->kw.vh_library) || vp_iskw(P, P->kw.vh_use)) {
            KA_GUARD(g2, 100);
            while (!vp_isop(P, ";") && vp_ctyp(P) != TK_TOK_EOF && g2--)
                vp_adv(P);
            vp_mop(P, ";");
            continue;
        }

        /* Entity → MODULE node */
        if (vp_iskw(P, P->kw.entity)) {
            last_mod = vp_ent(P);
            vp_achld(P, root, last_mod);
            continue;
        }

        /* Architecture → fills in the last entity's MODULE node */
        if (vp_iskw(P, P->kw.architecture)) {
            if (last_mod != 0) {
                vp_arch(P, last_mod);
            } else {
                /* Orphan architecture — create a module for it */
                uint32_t mod = vp_alloc(P, TK_AST_MODULE);
                vp_arch(P, mod);
                vp_achld(P, root, mod);
            }
            continue;
        }

        /* Skip unknown top-level */
        vp_adv(P);
    }

    printf("takahe: %u VHDL AST nodes, %u errors\n",
           P->n_node - 1, P->n_err);

    return (int)P->n_err;
}
