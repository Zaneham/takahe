/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_lex.c -- SystemVerilog lexer for Takahe
 *
 * Tokenises SystemVerilog source using keyword and operator
 * tables loaded from sv_tok.def. The lexer itself doesn't
 * know what a SystemVerilog keyword is -- it just knows how
 * to match identifiers against a sorted table.
 *
 * Like a postal worker who sorts letters by postcode without
 * knowing where any of the addresses actually are.
 *
 * JPL Power of 10: bounded loops, no alloc, no recursion.
 */

#include "takahe.h"

/* ---- Character classification ---- */

static int lx_alph(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static int lx_digt(char c)
{
    return c >= '0' && c <= '9';
}

static int lx_anum(char c)
{
    return lx_alph(c) || lx_digt(c);
}

static int lx_isws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* ---- Emit token ---- */

static void
lx_emit(tk_lex_t *L, tk_toktype_t type, uint16_t sub,
        uint32_t off, uint16_t len, uint32_t line, uint16_t col)
{
    tk_token_t *t;

    if (L->n_tok >= L->max_tok) return;  /* pool full, sentinel */

    t = &L->tokens[L->n_tok++];
    t->type = type;
    t->sub  = sub;
    t->off  = off;
    t->len  = len;
    t->line = line;
    t->col  = col;
}

/* ---- Intern source text into string pool ---- */

static uint32_t
lx_sint(tk_lex_t *L, const char *s, uint16_t len)
{
    uint32_t off;

    if (L->str_len + len + 1 > L->str_max) return 0;

    off = L->str_len;
    memcpy(L->strs + off, s, len);
    L->strs[off + len] = '\0';
    L->str_len += len + 1;

    return off;
}

/* ---- Keyword lookup (binary search on sorted table) ---- */

static int
lx_kwlk(const tk_lex_t *L, const char *s, uint16_t len)
{
    int lo = 0, hi = (int)L->n_kwd - 1;

    KA_GUARD(g, 20);
    while (lo <= hi && g--) {
        int mid = (lo + hi) / 2;
        const char *kw = L->strs + L->kwds[mid].name_off;
        int cmp = strncmp(s, kw, len);

        if (cmp == 0) {
            /* Match length too -- "int" shouldn't match "integer" */
            uint16_t klen = L->kwds[mid].name_len;
            if (len < klen) cmp = -1;
            else if (len > klen) cmp = 1;
            else return mid;
        }

        if (cmp < 0) hi = mid - 1;
        else          lo = mid + 1;
    }

    return -1;  /* not a keyword */
}

/* ---- Operator match (greedy, longest first) ---- */

static int
lx_opm(const tk_lex_t *L, const char *s, uint32_t rem)
{
    uint32_t i;

    for (i = 0; i < L->n_op; i++) {
        const tk_opdef_t *o = &L->ops[i];
        if (o->chars_len > rem) continue;

        const char *oc = L->strs + o->chars_off;
        if (memcmp(s, oc, o->chars_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

/* ---- Main lexer ---- */

int
tk_lex(tk_lex_t *L, const char *src, uint32_t len)
{
    uint32_t pos = 0;
    uint32_t line = 1;
    uint16_t col = 1;
    char c;

    if (!L || !src) return -1;

    L->src     = src;
    L->src_len = len;
    L->n_tok   = 0;
    L->n_err   = 0;

    KA_GUARD(g, TK_MAX_TOKENS + 1000);
    while (pos < len && g--) {
        c = src[pos];

        /* ---- Whitespace ---- */
        if (lx_isws(c)) {
            pos++; col++;
            continue;
        }

        /* ---- Newline ---- */
        if (c == '\n') {
            pos++; line++; col = 1;
            continue;
        }

        /* ---- Verilog attribute (* ... *) ----
         * Synthesis hints like (* parallel_case *) or (* keep *).
         * Skip the entire attribute for now. We'll want to parse
         * these properly in Tier 4 when they affect mapping, but
         * for now they just need to not break the lexer. */
        if (c == '(' && pos + 1 < len && src[pos+1] == '*' &&
            (pos + 2 >= len || src[pos+2] != ')')) {
            /* It's (* not (*) -- skip to matching *) */
            pos += 2; col += 2;
            KA_GUARD(ga, 10000);
            while (pos + 1 < len && ga--) {
                if (src[pos] == '*' && src[pos+1] == ')') {
                    pos += 2; col += 2;
                    break;
                }
                if (src[pos] == '\n') { line++; col = 1; }
                else { col++; }
                pos++;
            }
            continue;
        }

        /* ---- Line comment ---- */
        if (c == '/' && pos + 1 < len && src[pos+1] == '/') {
            while (pos < len && src[pos] != '\n') pos++;
            continue;
        }

        /* ---- Block comment ---- */
        if (c == '/' && pos + 1 < len && src[pos+1] == '*') {
            pos += 2; col += 2;
            KA_GUARD(gc, 1000000);
            while (pos + 1 < len && gc--) {
                if (src[pos] == '*' && src[pos+1] == '/') {
                    pos += 2; col += 2;
                    break;
                }
                if (src[pos] == '\n') { line++; col = 1; }
                else { col++; }
                pos++;
            }
            continue;
        }

        /* ---- Preprocessor directive (`keyword) ---- */
        if (c == '`') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            while (pos < len && lx_anum(src[pos])) { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = lx_sint(L, src + start, tlen);
            lx_emit(L, TK_TOK_PREPROC, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- System task ($name) ---- */
        if (c == '$' && pos + 1 < len && lx_alph(src[pos+1])) {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            while (pos < len && lx_anum(src[pos])) { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = lx_sint(L, src + start, tlen);
            lx_emit(L, TK_TOK_SYSTASK, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- String literal ---- */
        if (c == '"') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            KA_GUARD(gs, 100000);
            while (pos < len && src[pos] != '"' && gs--) {
                if (src[pos] == '\\' && pos + 1 < len) {
                    pos += 2; col += 2;  /* escape sequence */
                } else {
                    if (src[pos] == '\n') { line++; col = 1; }
                    else { col++; }
                    pos++;
                }
            }
            if (pos < len) { pos++; col++; }  /* closing quote */
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = lx_sint(L, src + start, tlen);
            lx_emit(L, TK_TOK_STR_LIT, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- Unbased unsized literal ('0, '1, 'x, 'z) ---- */
        if (c == '\'' && pos + 1 < len &&
            (src[pos+1] == '0' || src[pos+1] == '1' ||
             src[pos+1] == 'x' || src[pos+1] == 'X' ||
             src[pos+1] == 'z' || src[pos+1] == 'Z')) {
            /* Check it's not a based literal ('b, 'h, etc.) */
            char nxt = src[pos+1];
            if (nxt != 'b' && nxt != 'B' && nxt != 'o' && nxt != 'O' &&
                nxt != 'd' && nxt != 'D' && nxt != 'h' && nxt != 'H' &&
                nxt != 's' && nxt != 'S') {
                uint32_t start = pos;
                uint16_t scol = col;
                pos += 2; col += 2;
                uint32_t off = lx_sint(L, src + start, 2);
                lx_emit(L, TK_TOK_INT_LIT, 0, off, 2, line, scol);
                continue;
            }
        }

        /* ---- Number literal ----
         * Handles: decimal, based (4'b1010, 8'hFF), real.
         * The ' character distinguishes based from plain decimal. */
        if (lx_digt(c)) {
            uint32_t start = pos;
            uint16_t scol = col;

            /* Eat digits (size prefix or plain decimal) */
            while (pos < len && (lx_digt(src[pos]) || src[pos] == '_')) {
                pos++; col++;
            }

            /* Based literal: <size>'[sS]<base><digits> */
            if (pos < len && src[pos] == '\'') {
                pos++; col++;
                /* Optional sign */
                if (pos < len && (src[pos] == 's' || src[pos] == 'S')) {
                    pos++; col++;
                }
                /* Base letter */
                if (pos < len && (src[pos] == 'b' || src[pos] == 'B' ||
                                  src[pos] == 'o' || src[pos] == 'O' ||
                                  src[pos] == 'd' || src[pos] == 'D' ||
                                  src[pos] == 'h' || src[pos] == 'H')) {
                    pos++; col++;
                }
                /* Verilog allows whitespace between base letter
                 * and digits: 32'h 0000_0000 is valid. Because
                 * of course it is. IEEE 1800-2017 section 5.7.1. */
                while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) {
                    pos++; col++;
                }
                /* Digits (hex allows a-f, all allow x/z/?) */
                while (pos < len &&
                       (lx_anum(src[pos]) || src[pos] == '_' ||
                        src[pos] == '?' || src[pos] == 'x' ||
                        src[pos] == 'X' || src[pos] == 'z' ||
                        src[pos] == 'Z')) {
                    pos++; col++;
                }
            }
            /* Real literal: digits.digits or digits e/E [+-] digits */
            else if (pos < len && src[pos] == '.' &&
                     pos + 1 < len && lx_digt(src[pos+1])) {
                pos++; col++;
                while (pos < len && (lx_digt(src[pos]) || src[pos] == '_')) {
                    pos++; col++;
                }
                /* Exponent */
                if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
                    pos++; col++;
                    if (pos < len && (src[pos] == '+' || src[pos] == '-')) {
                        pos++; col++;
                    }
                    while (pos < len && lx_digt(src[pos])) {
                        pos++; col++;
                    }
                }
                uint16_t tlen = (uint16_t)(pos - start);
                uint32_t off = lx_sint(L, src + start, tlen);
                lx_emit(L, TK_TOK_REAL_LIT, 0, off, tlen, line, scol);
                continue;
            }

            uint16_t tlen = (uint16_t)(pos - start);
            if (tlen > 0) {
                uint32_t off = lx_sint(L, src + start, tlen);
                lx_emit(L, TK_TOK_INT_LIT, 0, off, tlen, line, scol);
            }
            continue;
        }

        /* ---- Identifier or keyword ---- */
        if (lx_alph(c)) {
            uint32_t start = pos;
            uint16_t scol = col;

            while (pos < len && lx_anum(src[pos])) { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);

            /* Check if it's a keyword */
            int kwi = lx_kwlk(L, src + start, tlen);
            if (kwi >= 0) {
                uint32_t off = lx_sint(L, src + start, tlen);
                lx_emit(L, TK_TOK_KWD, (uint16_t)kwi,
                         off, tlen, line, scol);
            } else {
                uint32_t off = lx_sint(L, src + start, tlen);
                lx_emit(L, TK_TOK_IDENT, 0, off, tlen, line, scol);
            }
            continue;
        }

        /* ---- Escaped identifier (\name) ---- */
        if (c == '\\') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            while (pos < len && !lx_isws(src[pos]) && src[pos] != '\n') {
                pos++; col++;
            }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = lx_sint(L, src + start, tlen);
            lx_emit(L, TK_TOK_IDENT, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- Operator ---- */
        {
            uint16_t scol = col;
            uint32_t rem = len - pos;
            int oi = lx_opm(L, src + pos, rem);

            if (oi >= 0) {
                uint16_t olen = L->ops[oi].chars_len;
                uint32_t off = lx_sint(L, src + pos, olen);
                lx_emit(L, TK_TOK_OP, (uint16_t)oi,
                         off, olen, line, scol);
                pos += olen;
                col = (uint16_t)(col + olen);
                continue;
            }
        }

        /* ---- Unknown character ---- */
        pos++; col++;
        L->n_err++;
    }

    /* EOF token */
    lx_emit(L, TK_TOK_EOF, 0, 0, 0, line, col);

    return (L->n_err > 0) ? -1 : 0;
}

/* ---- Utility ---- */

const char *
tk_tokstr(tk_toktype_t t)
{
    switch (t) {
    case TK_TOK_EOF:      return "EOF";
    case TK_TOK_IDENT:    return "IDENT";
    case TK_TOK_KWD:      return "KWD";
    case TK_TOK_INT_LIT:  return "INT";
    case TK_TOK_REAL_LIT: return "REAL";
    case TK_TOK_STR_LIT:  return "STR";
    case TK_TOK_OP:       return "OP";
    case TK_TOK_PREPROC:  return "PREPROC";
    case TK_TOK_SYSTASK:  return "SYSTASK";
    case TK_TOK_COMMENT:  return "COMMENT";
    case TK_TOK_ERROR:    return "ERROR";
    default:               return "?";
    }
}

const char *
tk_kwstr(const tk_lex_t *L, uint16_t id)
{
    if (!L || id >= L->n_kwd) return "?";
    return L->strs + L->kwds[id].name_off;
}

const char *
tk_opstr(const tk_lex_t *L, uint16_t id)
{
    if (!L || id >= L->n_op) return "?";
    return L->strs + L->ops[id].name_off;
}
