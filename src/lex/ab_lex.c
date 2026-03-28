/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * ab_lex.c -- ABEL-HDL lexer for Takahe
 *
 * Tokenises ABEL-HDL source using tables loaded from
 * abel_tok.def. Third frontend after SystemVerilog and VHDL.
 *
 * ABEL keywords are case-insensitive but identifiers are
 * case-sensitive. Comments use " (to end of line or matching ")
 * and // (to end of line). Numbers use ^b ^o ^d ^h prefixes.
 * Special constants are dot-delimited (.X. .C. .Z. etc).
 *
 * Synario ABEL-HDL Reference, Data I/O, 1995.
 * Retrieved from bitsavers.org before the last copy vanished.
 */

#include "takahe.h"
#include <ctype.h>

/* ---- Character helpers ---- */

static int ab_alph(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_' || c == '~';
}

static int ab_digt(char c)
{
    return c >= '0' && c <= '9';
}

static int ab_anum(char c)
{
    return ab_alph(c) || ab_digt(c);
}

static int ab_hexd(char c)
{
    return ab_digt(c) ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int ab_isws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* ---- Token emit and string intern ----
 * Same logic as tk_lex.c, kept local to avoid cross-TU
 * coupling. Seven lines each, not worth a shared header. */

static void
ab_emit(tk_lex_t *L, tk_toktype_t type, uint16_t sub,
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

static uint32_t
ab_sint(tk_lex_t *L, const char *s, uint16_t len)
{
    uint32_t off;
    if (L->str_len + len + 1 > L->str_max) return 0;
    off = L->str_len;
    memcpy(L->strs + off, s, len);
    L->strs[off + len] = '\0';
    L->str_len += len + 1;
    return off;
}

/* ---- Case-insensitive keyword lookup ----
 * ABEL keywords are case-insensitive. The def file stores
 * them lowercase; we normalise to lowercase for comparison. */

static int
ab_kwlk(const tk_lex_t *L, const char *s, uint16_t len)
{
    uint32_t i;

    for (i = 0; i < L->n_kwd; i++) {
        const char *kw = L->strs + L->kwds[i].name_off;
        uint16_t klen = L->kwds[i].name_len;
        uint16_t j;

        if (klen != len) continue;

        for (j = 0; j < len; j++) {
            char a = (char)tolower((unsigned char)s[j]);
            char b = (char)tolower((unsigned char)kw[j]);
            if (a != b) break;
        }
        if (j == len) return (int)i;
    }

    return -1;
}

/* ---- Operator match (greedy, longest first) ---- */

static int
ab_opm(const tk_lex_t *L, const char *s, uint32_t rem)
{
    uint32_t i;
    int best = -1;
    uint16_t blen = 0;

    /* longest match wins — !$ must beat ! */
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

/* ---- Main ABEL lexer ---- */

int
ab_lex(tk_lex_t *L, const char *src, uint32_t len)
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
        if (ab_isws(c)) { pos++; col++; continue; }

        /* ---- Newline ---- */
        if (c == '\n') { pos++; line++; col = 1; continue; }

        /* ---- // comment (to end of line) ---- */
        if (c == '/' && pos + 1 < len && src[pos+1] == '/') {
            while (pos < len && src[pos] != '\n') pos++;
            continue;
        }

        /* ---- " comment (to matching " or end of line) ---- */
        if (c == '"') {
            pos++; col++;
            KA_GUARD(gc, 10000);
            while (pos < len && gc--) {
                if (src[pos] == '"' || src[pos] == '\n') break;
                pos++; col++;
            }
            if (pos < len && src[pos] == '"') { pos++; col++; }
            continue;
        }

        /* ---- @ directive ---- */
        if (c == '@') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            while (pos < len && ab_anum(src[pos])) { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = ab_sint(L, src + start, tlen);
            ab_emit(L, TK_TOK_PREPROC, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- Special constant (.X. .C. .Z. etc) ----
         * Dot-letter-dot pattern. The letter determines
         * which constant. Case-insensitive. */
        if (c == '.' && pos + 2 < len && src[pos+2] == '.' &&
            ab_alph(src[pos+1])) {
            /* could be .SVn. (super voltage) — check for digits */
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;  /* skip first dot */
            while (pos < len && src[pos] != '.') {
                if (!ab_anum(src[pos])) break;
                pos++; col++;
            }
            if (pos < len && src[pos] == '.') { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = ab_sint(L, src + start, tlen);
            ab_emit(L, TK_TOK_INT_LIT, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- String literal ('...') ----
         * ABEL uses single quotes for strings, with backslash
         * escape for embedded quotes. */
        if (c == '\'' || c == '`') {
            uint32_t start = pos;
            uint16_t scol = col;
            pos++; col++;
            KA_GUARD(gs, 100000);
            while (pos < len && gs--) {
                if (src[pos] == '\\' && pos + 1 < len) {
                    pos += 2; col += 2;
                } else if (src[pos] == '\'' || src[pos] == '`') {
                    pos++; col++;
                    break;
                } else if (src[pos] == '\n') {
                    break;  /* unterminated */
                } else {
                    pos++; col++;
                }
            }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = ab_sint(L, src + start, tlen);
            ab_emit(L, TK_TOK_STR_LIT, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- Number literal ----
         * ^b = binary, ^o = octal, ^d = decimal, ^h = hex.
         * Plain digits = decimal (default base). */
        if (c == '^' && pos + 1 < len) {
            char base = (char)tolower((unsigned char)src[pos+1]);
            if (base == 'b' || base == 'o' || base == 'd' || base == 'h') {
                uint32_t start = pos;
                uint16_t scol = col;
                pos += 2; col += 2;
                KA_GUARD(gn, 200);
                while (pos < len && gn--) {
                    if (base == 'h' && ab_hexd(src[pos])) { pos++; col++; }
                    else if (ab_digt(src[pos])) { pos++; col++; }
                    else break;
                }
                uint16_t tlen = (uint16_t)(pos - start);
                uint32_t off = ab_sint(L, src + start, tlen);
                ab_emit(L, TK_TOK_INT_LIT, 0, off, tlen, line, scol);
                continue;
            }
        }

        /* Plain decimal number */
        if (ab_digt(c)) {
            uint32_t start = pos;
            uint16_t scol = col;
            while (pos < len && ab_digt(src[pos])) { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);
            uint32_t off = ab_sint(L, src + start, tlen);
            ab_emit(L, TK_TOK_INT_LIT, 0, off, tlen, line, scol);
            continue;
        }

        /* ---- Identifier or keyword ---- */
        if (ab_alph(c)) {
            uint32_t start = pos;
            uint16_t scol = col;
            while (pos < len && ab_anum(src[pos])) { pos++; col++; }
            uint16_t tlen = (uint16_t)(pos - start);

            int kwi = ab_kwlk(L, src + start, tlen);
            uint32_t off = ab_sint(L, src + start, tlen);
            if (kwi >= 0)
                ab_emit(L, TK_TOK_KWD, (uint16_t)kwi,
                         off, tlen, line, scol);
            else
                ab_emit(L, TK_TOK_IDENT, 0,
                         off, tlen, line, scol);
            continue;
        }

        /* ---- Operator ---- */
        {
            uint16_t scol = col;
            uint32_t rem = len - pos;
            int oi = ab_opm(L, src + pos, rem);

            if (oi >= 0) {
                uint16_t olen = L->ops[oi].chars_len;
                uint32_t off = ab_sint(L, src + pos, olen);
                ab_emit(L, TK_TOK_OP, (uint16_t)oi,
                         off, olen, line, scol);
                pos += olen;
                col = (uint16_t)(col + olen);
                continue;
            }
        }

        /* ---- Unknown ---- */
        pos++; col++;
        L->n_err++;
    }

    ab_emit(L, TK_TOK_EOF, 0, 0, 0, line, col);

    return (L->n_err > 0) ? -1 : 0;
}
