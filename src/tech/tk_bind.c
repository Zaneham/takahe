/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_bind.c -- Cell binding for Takahe
 *
 * Maps each RTL cell type to the best-matching library gate.
 * Classifies Liberty cells by their function strings: (A&B)
 * is AND, (!A|!B) is NAND, etc. Picks the smallest-area
 * variant of each type.
 *
 * The binding table is just a lookup: RT_AND → cell index 42.
 * The hard work is classifying function strings, which is
 * substring matching rather than boolean algebra because
 * Liberty function expressions are normalised enough that
 * pattern matching works and saves us writing a parser
 * for a parser for a parser.
 */

#include "takahe.h"

/* ---- Function string pattern matching ---- */

static int
fn_match(const char *fn, uint16_t len, const char *pat)
{
    uint16_t plen = (uint16_t)strlen(pat);
    uint16_t i;
    if (len < plen) return 0;
    for (i = 0; i <= len - plen; i++) {
        if (memcmp(fn + i, pat, plen) == 0) return 1;
    }
    return 0;
}

static int
fn_is(const char *fn, uint16_t len, const char *exact)
{
    uint16_t elen = (uint16_t)strlen(exact);
    while (len > 0 && (*fn == ' ' || *fn == '(')) { fn++; len--; }
    while (len > 0 && (fn[len-1] == ' ' || fn[len-1] == ')')) len--;
    return (len == elen && memcmp(fn, exact, elen) == 0);
}

/* Classify a combinational cell by its output function string */

/* Normalize a Liberty function string for matching.
 * Strips spaces, maps pin names to canonical A/B/C/S,
 * replaces * with & and + with |.
 * GF180 uses A1,A2,I; ASAP7 uses "!A * !B" with spaces;
 * SKY130 uses A,B with &|. After normalization they all
 * look the same. Like translating Sumerian, Akkadian, and
 * Greek prices into the same currency. */

static uint16_t
fn_norm(const char *fn, uint16_t fl, char *out, uint16_t cap)
{
    uint16_t i, o = 0;
    for (i = 0; i < fl && o < cap - 1; i++) {
        char c = fn[i];
        if (c == ' ' || c == '\t') continue; /* strip spaces */
        if (c == '*') c = '&';  /* normalize AND */
        if (c == '+') c = '|';  /* normalize OR */
        /* Map pin names: A1→A, A2→B, B1→C, B2→D, I→A */
        if (c == 'I' && (i + 1 >= fl || !((fn[i+1] >= 'A' && fn[i+1] <= 'Z') ||
            (fn[i+1] >= 'a' && fn[i+1] <= 'z') || fn[i+1] == '_'))) {
            /* Standalone I → A (for inverters like GF180 "(!I)") */
            c = 'A';
        }
        if (c == 'A' && i + 1 < fl && fn[i+1] == '1') { c = 'A'; i++; }
        else if (c == 'A' && i + 1 < fl && fn[i+1] == '2') { c = 'B'; i++; }
        else if (c == 'B' && i + 1 < fl && fn[i+1] == '1') { c = 'C'; i++; }
        else if (c == 'B' && i + 1 < fl && fn[i+1] == '2') { c = 'D'; i++; }
        out[o++] = c;
    }
    out[o] = '\0';
    return o;
}

static rt_ctype_t
fn_cls(const lb_lib_t *lib, const lb_cell_t *cell)
{
    uint8_t j;
    const char *fn = NULL;
    uint16_t fl = 0;
    char nb[128]; /* normalized buffer */
    uint16_t nl;

    for (j = 0; j < cell->n_pin; j++) {
        if (cell->pins[j].dir == LB_DIR_OUT &&
            cell->pins[j].func_len > 0) {
            fn = lib->strs + cell->pins[j].func_off;
            fl = cell->pins[j].func_len;
            break;
        }
    }
    if (!fn || fl == 0) return RT_CELL_COUNT;

    /* Normalize for matching */
    nl = fn_norm(fn, fl, nb, 128);
    fn = nb;
    fl = nl;

    /* After normalization: & = AND, | = OR, A/B/C = pins.
     * All PDKs speak the same language now. */
    if (cell->n_in == 2) {
        if (fn_is(fn, fl, "A&B"))  return RT_AND;
        if (fn_is(fn, fl, "A|B") || fn_match(fn, fl, "(A)|(B)"))
            return RT_OR;
        if (fn_match(fn, fl, "A^B") ||
            (fn_match(fn, fl, "A&!B") && fn_match(fn, fl, "!A&B")))
            return RT_XOR;
        if (fn_is(fn, fl, "!A|!B") ||
            fn_match(fn, fl, "!(A&B)") ||
            fn_is(fn, fl, "(!A)|(!B)"))
            return RT_NAND;
        if (fn_is(fn, fl, "!A&!B") ||
            fn_match(fn, fl, "!(A|B)") ||
            fn_is(fn, fl, "(!A)&(!B)"))
            return RT_NOR;
        if (fn_match(fn, fl, "!(A^B)") ||
            (fn_match(fn, fl, "!A&!B") && fn_match(fn, fl, "A&B")))
            return RT_XNOR;
    }

    if (cell->n_in == 1) {
        if (fn_is(fn, fl, "!A")) return RT_NOT;
        if (fn_is(fn, fl, "A"))  return RT_BUF;
    }

    if (cell->n_in == 3 &&
        fn_match(fn, fl, "&!S") &&
        fn_match(fn, fl, "&S") &&
        !fn_match(fn, fl, "!A"))
        return RT_MUX;

    return RT_CELL_COUNT;
}

/* ---- Public: bind library cells to RTL types ---- */

int
mp_bind(const lb_lib_t *lib, mp_bind_t *tbl)
{
    uint32_t i;
    int bound = 0;

    memset(tbl, 0, (size_t)RT_CELL_COUNT * sizeof(mp_bind_t));

    for (i = 0; i < lib->n_cell; i++) {
        const lb_cell_t *cell = &lib->cells[i];
        rt_ctype_t ct;

        if (cell->kind == LB_DFF) {
            ct = cell->rst_pin != 0xFF ? RT_DFFR : RT_DFF;
        } else if (cell->kind == LB_DLAT) {
            ct = RT_DLAT;
        } else if (cell->kind == LB_TIE) {
            ct = RT_CONST;
        } else {
            ct = fn_cls(lib, cell);
        }

        if (ct >= RT_CELL_COUNT) continue;

        if (!tbl[ct].valid || cell->area < lib->cells[tbl[ct].cell_idx].area) {
            tbl[ct].cell_idx = i;
            tbl[ct].valid = 1;
            bound++;
        }
    }

    if (!tbl[RT_ASSIGN].valid && tbl[RT_BUF].valid)
        tbl[RT_ASSIGN] = tbl[RT_BUF];

    printf("takahe: bound %d RTL types to library cells\n", bound);
    return bound;
}
