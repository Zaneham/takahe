/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * vh_lex.c -- VHDL lexer for Takahe
 *
 * Tokenises VHDL source using keyword and operator tables
 * loaded from vhdl_tok.def. VHDL is case-insensitive, so
 * identifiers are lowercased before keyword lookup.
 *
 * VHDL was designed by the US Department of Defense in 1980.
 * It shows. The syntax has the ergonomics of a procurement
 * form and the readability of a military specification. But
 * it describes hardware precisely, which is more than can be
 * said for most programming languages. And unlike the $600
 * toilet seats, the language itself is free.
 *
 * JPL Power of 10: bounded loops, no alloc, no recursion.
 */

#include "takahe.h"
#include <ctype.h>

/* ---- Character classification ---- */

static int vh_alph(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static int vh_digt(char c)
{
    return c >= '0' && c <= '9';
}

static int vh_anum(char c)
{
    return vh_alph(c) || vh_digt(c);
}

static int vh_isws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* ---- Lowercase a span into the string pool ----
 * VHDL is case-insensitive. "SIGNAL" = "signal" = "Signal".
 * The Sumerians also didn't distinguish upper and lower
 * cuneiform — they had more important things to worry about,
 * like the price of copper from Ea-nasir. */

static uint32_t
vh_sint(tk_lex_t *L, const char *s, uint16_t len, int do_lower)
{
    uint32_t off;

    if (L->str_len + len + 1 > L->str_max) return 0;

    off = L->str_len;
    if (do_lower) {
        uint16_t i;
        for (i = 0; i < len; i++)
            L->strs[off + i] = (char)tolower((unsigned char)s[i]);
    } else {
        memcpy(L->strs + off, s, len);
    }
    L->strs[off + len] = '\0';
    L->str_len += len + 1;

    return off;
}

/* ---- Emit token ---- */

static void
vh_emit(tk_lex_t *L, tk_toktype_t type, uint16_t sub,
        uint32_t off, uint16_t len, uint32_t line, uint16_t col)
{
    tk_token_t *t;
    if (L->n_tok >= L->max_tok) return;
    t = &L->tokens[L->n_tok++];
    t->type = type;
    t->sub  = sub;
    t->off  = off;
    t->len  = len;
    t->line = line;
    t->col  = col;
}

/* ---- Keyword lookup (binary search, case-insensitive) ---- */

static int
vh_kwlk(const tk_lex_t *L, const char *s, uint16_t len)
{
    int lo = 0, hi = (int)L->n_kwd - 1;

    KA_GUARD(g, 20);
    while (lo <= hi && g--) {
        int mid = (lo + hi) / 2;
        const char *kw = L->strs + L->kwds[mid].name_off;
        int cmp = strncmp(s, kw, len);

        if (cmp == 0) {
            uint16_t klen = L->kwds[mid].name_len;
            if (len < klen) cmp = -1;
            else if (len > klen) cmp = 1;
            else return mid;
        }

        if (cmp < 0) hi = mid - 1;
        else          lo = mid + 1;
    }

    return -1;
}

/* ---- Operator match (greedy, longest first) ---- */

static int
vh_opm(const tk_lex_t *L, const char *s, uint32_t rem)
{
    int best = -1;
    uint16_t blen = 0;
    uint32_t i;

    /* VHDL operators are short (1-3 chars). Find longest match. */
    for (i = 0; i < L->n_op; i++) {
        const tk_opdef_t *o = &L->ops[i];
        if (o->chars_len > rem) continue;
        if (o->chars_len <= blen) continue;

        const char *oc = L->strs + o->chars_off;
        if (memcmp(s, oc, o->chars_len) == 0) {
            best = (int)i;
            blen = o->chars_len;
        }
    }

    return best;
}

/* ---- Main VHDL lexer ---- */

int
vh_lex(tk_lex_t *L, const char *src, uint32_t len)
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
        if (vh_isws(c)) {
            pos++; col++;
            continue;
        }

        /* ---- Newline ---- */
        if (c == '\n') {
            pos++; line++; col = 1;
            continue;
        }

        /* ---- VHDL comment: -- to end of line ---- */
        if (c == '-' && pos + 1 < len && src[pos+1] == '-') {
            while (pos < len && src[pos] != '\n') pos++;
            continue;
        }

        /* ---- Block comment (VHDL-2008): delimited comment ---- */
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

        /* ---- String literal "..." ---- */
        if (c == '"') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            KA_GUARD(gs, 100000);
            while (pos < len && gs--) {
                if (src[pos] == '"') {
                    pos++; col++;
                    /* VHDL "" is escaped quote inside string */
                    if (pos < len && src[pos] == '"') {
                        pos++; col++;
                        continue;
                    }
                    break;
                }
                if (src[pos] == '\n') { line++; col = 1; }
                else { col++; }
                pos++;
            }
            {
                uint16_t tlen = (uint16_t)(pos - start);
                uint32_t off = vh_sint(L, src + start, tlen, 0);
                vh_emit(L, TK_TOK_STR_LIT, 0, off, tlen, line, scol);
            }
            continue;
        }

        /* ---- Character literal 'x' ---- */
        if (c == '\'' && pos + 2 < len && src[pos+2] == '\'' &&
            vh_alph(src[pos+1])) {
            /* But not if preceded by identifier (attribute tick) */
            uint32_t off;
            uint16_t scol = col;
            off = vh_sint(L, src + pos, 3, 0);
            vh_emit(L, TK_TOK_INT_LIT, 0, off, 3, line, scol);
            pos += 3; col += 3;
            continue;
        }

        /* ---- Bit string literal: B"1010", X"FF", O"77" ----
         * Also handles D"123" (VHDL-2008). */
        if ((c == 'B' || c == 'b' || c == 'O' || c == 'o' ||
             c == 'X' || c == 'x' || c == 'D' || c == 'd') &&
            pos + 1 < len && src[pos+1] == '"') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos += 2; col += 2;
            KA_GUARD(gbs, 10000);
            while (pos < len && src[pos] != '"' && gbs--) {
                pos++; col++;
            }
            if (pos < len) { pos++; col++; }
            {
                uint16_t tlen = (uint16_t)(pos - start);
                uint32_t off = vh_sint(L, src + start, tlen, 0);
                vh_emit(L, TK_TOK_INT_LIT, 0, off, tlen, line, scol);
            }
            continue;
        }

        /* ---- Based literal: 16#FF# or 2#1010# ----
         * Format: base#digits[.digits]#[exponent] */
        if (vh_digt(c)) {
            uint32_t start = pos;
            uint16_t scol = col;

            /* Eat decimal digits */
            while (pos < len && (vh_digt(src[pos]) || src[pos] == '_')) {
                pos++; col++;
            }

            /* Based literal? */
            if (pos < len && src[pos] == '#') {
                pos++; col++;
                /* Eat based digits (hex) */
                KA_GUARD(gb, 10000);
                while (pos < len && src[pos] != '#' && gb--) {
                    pos++; col++;
                }
                if (pos < len && src[pos] == '#') {
                    pos++; col++;
                }
                /* Optional exponent */
                if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
                    pos++; col++;
                    if (pos < len && (src[pos] == '+' || src[pos] == '-')) {
                        pos++; col++;
                    }
                    while (pos < len && vh_digt(src[pos])) {
                        pos++; col++;
                    }
                }
            }
            /* Real literal? */
            else if (pos < len && src[pos] == '.' &&
                     pos + 1 < len && vh_digt(src[pos+1])) {
                pos++; col++;
                while (pos < len && (vh_digt(src[pos]) || src[pos] == '_')) {
                    pos++; col++;
                }
                if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
                    pos++; col++;
                    if (pos < len && (src[pos] == '+' || src[pos] == '-')) {
                        pos++; col++;
                    }
                    while (pos < len && vh_digt(src[pos])) {
                        pos++; col++;
                    }
                }
            }
            /* else: plain integer */

            {
                uint16_t tlen = (uint16_t)(pos - start);
                uint32_t off = vh_sint(L, src + start, tlen, 0);
                vh_emit(L, TK_TOK_INT_LIT, 0, off, tlen, line, scol);
            }
            continue;
        }

        /* ---- Identifier or keyword ---- */
        if (vh_alph(c)) {
            uint32_t start = pos;
            uint16_t scol = col;

            while (pos < len && vh_anum(src[pos])) {
                pos++; col++;
            }

            {
                uint16_t tlen = (uint16_t)(pos - start);
                /* Lowercase for keyword lookup */
                uint32_t off = vh_sint(L, src + start, tlen, 1);
                int ki = vh_kwlk(L, L->strs + off, tlen);

                if (ki >= 0) {
                    vh_emit(L, TK_TOK_KWD, (uint16_t)ki,
                            off, tlen, line, scol);
                } else {
                    vh_emit(L, TK_TOK_IDENT, 0, off, tlen, line, scol);
                }
            }
            continue;
        }

        /* ---- Extended identifier \...\ ---- */
        if (c == '\\') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            KA_GUARD(ge, 10000);
            while (pos < len && src[pos] != '\\' && ge--) {
                pos++; col++;
            }
            if (pos < len) { pos++; col++; }
            {
                uint16_t tlen = (uint16_t)(pos - start);
                uint32_t off = vh_sint(L, src + start, tlen, 0);
                vh_emit(L, TK_TOK_IDENT, 0, off, tlen, line, scol);
            }
            continue;
        }

        /* ---- Operator ---- */
        {
            int oi = vh_opm(L, src + pos, len - pos);
            if (oi >= 0) {
                uint16_t olen = L->ops[oi].chars_len;
                uint32_t off = vh_sint(L, src + pos, olen, 0);
                vh_emit(L, TK_TOK_OP, (uint16_t)oi,
                        off, olen, line, col);
                pos += olen;
                col += olen;
                continue;
            }
        }

        /* ---- Tick (attribute or qualified expression) ---- */
        if (c == '\'') {
            uint32_t off = vh_sint(L, "'", 1, 0);
            vh_emit(L, TK_TOK_OP, 0, off, 1, line, col);
            pos++; col++;
            continue;
        }

        /* ---- Unknown character ---- */
        {
            char unk[2] = { c, '\0' };
            uint32_t off = vh_sint(L, unk, 1, 0);
            vh_emit(L, TK_TOK_ERROR, 0, off, 1, line, col);
            L->n_err++;
            pos++; col++;
        }
    }

    return (int)L->n_err;
}
