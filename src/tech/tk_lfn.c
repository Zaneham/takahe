/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_lfn.c -- Liberty function expressions into truth tables
 *
 * fn_cls recognises a cell by matching its function string against nineteen
 * hardcoded patterns. Fine for and2, useless for a31o, and a31o is most of
 * what a real library is made of.
 *
 * So parse the expression instead and evaluate it into the cd_cell_t truth
 * table the rest of the engine already speaks. Shunting-yard rather than
 * recursive descent because the house rules ban recursion, which is no great
 * loss here.
 *
 * A function naming something that isn't an input pin gets rejected
 * outright. Sequential cells say things like Q = IQ, and a wrong truth table
 * is worse than none.
 */

#include "takahe.h"
#include <string.h>

#define LF_MAX_TOK  192   /* tokens in one function expression */
#define LF_MAX_STK   64   /* evaluation stack depth            */

enum {
    LF_END = 0, LF_VAR, LF_CON, LF_NOT, LF_AND, LF_XOR, LF_OR,
    LF_LP, LF_RP
};

typedef struct {
    uint8_t kind;
    uint8_t val;   /* LF_VAR: input ordinal.  LF_CON: 0 or 1. */
} lf_tok_t;

/* ---- Operator precedence ----
 * Liberty binds ! tightest, then &, then ^, then |. */

static uint8_t
lf_prec(uint8_t k)
{
    switch ((int)k) {
    case LF_NOT: return 4;
    case LF_AND: return 3;
    case LF_XOR: return 2;
    case LF_OR:  return 1;
    default:     return 0;
    }
}

/* ---- Resolve an identifier to an input ordinal ----
 * Returns the position in inp[], not the library pin index, so
 * truth table columns line up with cd_cell_t pins. */

static int
lf_pidx(const lb_lib_t *lib, const lb_cell_t *cell, const uint8_t *inp,
        uint8_t n_in, const char *s, uint16_t n)
{
    uint8_t j;

    for (j = 0; j < n_in; j++) {
        const lb_pin_t *p = &cell->pins[inp[j]];
        if (p->name_len != n) continue;
        if (memcmp(lib->strs + p->name_off, s, (size_t)n) == 0)
            return (int)j;
    }
    return -1;
}

static int
lf_ident(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* ---- Infix to RPN ----
 * Liberty leaves AND implicit ("A1 B1" means A1&B1), so an operand
 * arriving straight after an operand inserts one.
 *
 * I spent a while on the Liberty grammar convinced I was missing
 * an operator, before working out that juxtaposition IS the
 * operator and the whitespace is just decoration. Every PDK
 * spells the explicit ones differently as well, so the tokeniser
 * takes & or * and | or +, and quietly stops caring. */

static int
lb_frpn(const lb_lib_t *lib, const lb_cell_t *cell, const uint8_t *inp,
        uint8_t n_in, const char *s, uint16_t len,
        lf_tok_t *out, uint16_t *outn)
{
    lf_tok_t ops[LF_MAX_TOK];
    uint16_t i = 0, on = 0;
    uint8_t sp = 0, prev = LF_END;
    KA_GUARD(g, LF_MAX_TOK * 8);

    while (i < len && g--) {
        char c = s[i];
        uint8_t k;
        lf_tok_t t;

        if (c == ' ' || c == '\t') { i++; continue; }

        /* Postfix negation binds to the operand just emitted */
        if (c == '\'') {
            if (on >= LF_MAX_TOK) return -1;
            out[on].kind = LF_NOT; out[on].val = 0; on++;
            prev = LF_VAR; i++; continue;
        }

        if (c == '(' || c == '!' || lf_ident(c)) {
            /* Implicit AND between two adjacent operands */
            if (prev == LF_VAR || prev == LF_CON || prev == LF_RP) {
                while (sp > 0 && ops[sp - 1].kind != LF_LP &&
                       lf_prec(ops[sp - 1].kind) >= lf_prec(LF_AND)) {
                    if (on >= LF_MAX_TOK) return -1;
                    out[on++] = ops[--sp];
                }
                if (sp >= LF_MAX_TOK) return -1;
                ops[sp].kind = LF_AND; ops[sp].val = 0; sp++;
            }
        }

        if (c == '(') {
            if (sp >= LF_MAX_TOK) return -1;
            ops[sp].kind = LF_LP; ops[sp].val = 0; sp++;
            prev = LF_LP; i++; continue;
        }

        if (c == ')') {
            while (sp > 0 && ops[sp - 1].kind != LF_LP) {
                if (on >= LF_MAX_TOK) return -1;
                out[on++] = ops[--sp];
            }
            if (sp == 0) return -1;      /* unbalanced */
            sp--;
            prev = LF_RP; i++; continue;
        }

        if (c == '!') {
            if (sp >= LF_MAX_TOK) return -1;
            ops[sp].kind = LF_NOT; ops[sp].val = 0; sp++;
            prev = LF_NOT; i++; continue;
        }

        k = 0;
        if (c == '&' || c == '*') k = LF_AND;
        else if (c == '|' || c == '+') k = LF_OR;
        else if (c == '^') k = LF_XOR;

        if (k != 0) {
            while (sp > 0 && ops[sp - 1].kind != LF_LP &&
                   lf_prec(ops[sp - 1].kind) >= lf_prec(k)) {
                if (on >= LF_MAX_TOK) return -1;
                out[on++] = ops[--sp];
            }
            if (sp >= LF_MAX_TOK) return -1;
            ops[sp].kind = k; ops[sp].val = 0; sp++;
            prev = k; i++; continue;
        }

        if (lf_ident(c)) {
            uint16_t st = i;
            int i_ord;
            while (i < len && lf_ident(s[i])) i++;

            /* Bare 0 and 1 are constants, everything else is a pin */
            if (i - st == 1 && (s[st] == '0' || s[st] == '1')) {
                t.kind = LF_CON;
                t.val = (uint8_t)(s[st] - '0');
            } else {
                i_ord = lf_pidx(lib, cell, inp, n_in, s + st,
                                (uint16_t)(i - st));
                if (i_ord < 0) return -1;   /* not an input pin */
                t.kind = LF_VAR;
                t.val = (uint8_t)i_ord;
            }
            if (on >= LF_MAX_TOK) return -1;
            out[on++] = t;
            prev = t.kind;
            continue;
        }

        return -1;   /* character not in the grammar */
    }

    while (sp > 0) {
        if (ops[sp - 1].kind == LF_LP) return -1;
        if (on >= LF_MAX_TOK) return -1;
        out[on++] = ops[--sp];
    }

    if (on == 0) return -1;
    *outn = on;
    return 0;
}

/* ---- Evaluate RPN for one input vector ---- */

static int
lb_frun(const lf_tok_t *r, uint16_t n, const int8_t *iv, int8_t *res)
{
    int8_t st[LF_MAX_STK];
    uint16_t i;
    uint8_t sp = 0;

    for (i = 0; i < n; i++) {
        int8_t a, b;

        switch ((int)r[i].kind) {
        case LF_VAR:
            if (sp >= LF_MAX_STK) return -1;
            st[sp++] = iv[r[i].val];
            break;
        case LF_CON:
            if (sp >= LF_MAX_STK) return -1;
            st[sp++] = (int8_t)r[i].val;
            break;
        case LF_NOT:
            if (sp < 1) return -1;
            st[sp - 1] = (int8_t)(st[sp - 1] ? 0 : 1);
            break;
        case LF_AND:
        case LF_OR:
        case LF_XOR:
            if (sp < 2) return -1;
            b = st[--sp];
            a = st[sp - 1];
            st[sp - 1] = (int8_t)(r[i].kind == LF_AND ? (a && b) :
                                  r[i].kind == LF_OR  ? (a || b) :
                                                        (a != b));
            break;
        default:
            return -1;
        }
    }

    if (sp != 1) return -1;
    *res = st[0];
    return 0;
}

/* ---- Public: find a library cell by name ---- */

const lb_cell_t *
lb_fcel(const lb_lib_t *lib, const char *name, uint16_t len)
{
    uint32_t i;

    if (!lib || !name) return NULL;

    for (i = 0; i < lib->n_cell; i++) {
        const lb_cell_t *c = &lib->cells[i];
        if (c->name_len != len) continue;
        if (memcmp(lib->strs + c->name_off, name, (size_t)len) == 0)
            return c;
    }
    return NULL;
}

/* ---- Public: intern a cell's truth table in a cd library ----
 * Deduplicates by name, so a netlist with four hundred nand2s
 * costs one table. Returns the cd index, or -1. */

int
lb_cdix(const lb_lib_t *lib, const lb_cell_t *cell, cd_lib_t *cd)
{
    uint32_t i;
    uint16_t nl;
    char nm[32];

    if (!lib || !cell || !cd) return -1;

    nl = cell->name_len < 31 ? cell->name_len : 31;
    memcpy(nm, lib->strs + cell->name_off, (size_t)nl);
    nm[nl] = '\0';

    for (i = 0; i < cd->n_cell; i++)
        if (strcmp(cd->cells[i].name, nm) == 0) return (int)i;

    if (cd->n_cell >= CD_MAX_CELLS) return -1;
    if (lb_fbld(lib, cell, &cd->cells[cd->n_cell]) != 0) return -1;

    cd->n_cell++;
    return (int)(cd->n_cell - 1);
}

/* ---- Public: build a truth table for a combinational cell ----
 * Returns 0 on success, -1 if the cell isn't expressible as one
 * (sequential, too wide, or a function that will not parse). */

int
lb_fbld(const lb_lib_t *lib, const lb_cell_t *cell, cd_cell_t *out)
{
    uint8_t  inp[CD_MAX_PINS], outp[CD_MAX_VALS];
    lf_tok_t rpn[CD_MAX_VALS][LF_MAX_TOK];
    uint16_t rlen[CD_MAX_VALS];
    uint8_t  n_in = 0, n_out = 0, j;
    uint32_t combo, n_combo;
    uint16_t nl;

    if (!lib || !cell || !out) return -1;

    for (j = 0; j < cell->n_pin; j++) {
        const lb_pin_t *p = &cell->pins[j];
        if (p->dir == LB_DIR_IN) {
            if (n_in >= CD_MAX_PINS) return -1;
            inp[n_in++] = j;
        } else if (p->dir == LB_DIR_OUT && p->func_len > 0) {
            if (n_out >= CD_MAX_VALS) return -1;
            outp[n_out++] = j;
        }
    }

    if (n_out == 0) return -1;
    if (n_in + n_out > CD_MAX_PINS) return -1;
    if (n_in > 0 && (1u << n_in) > CD_MAX_ROWS) return -1;

    for (j = 0; j < n_out; j++) {
        const lb_pin_t *p = &cell->pins[outp[j]];
        if (lb_frpn(lib, cell, inp, n_in, lib->strs + p->func_off,
                    p->func_len, rpn[j], &nl) != 0)
            return -1;
        rlen[j] = nl;
    }

    memset(out, 0, sizeof(*out));
    out->radix = 2;
    out->n_in  = n_in;
    out->n_out = n_out;
    out->n_pin = (uint8_t)(n_in + n_out);

    nl = cell->name_len < 31 ? cell->name_len : 31;
    memcpy(out->name, lib->strs + cell->name_off, (size_t)nl);
    out->name[nl] = '\0';

    for (j = 0; j < n_in; j++) {
        const lb_pin_t *p = &cell->pins[inp[j]];
        uint16_t l = p->name_len < 15 ? p->name_len : 15;
        memcpy(out->pins[j], lib->strs + p->name_off, (size_t)l);
        out->pins[j][l] = '\0';
        out->pdir[j] = 1;
    }
    for (j = 0; j < n_out; j++) {
        const lb_pin_t *p = &cell->pins[outp[j]];
        uint16_t l = p->name_len < 15 ? p->name_len : 15;
        memcpy(out->pins[n_in + j], lib->strs + p->name_off, (size_t)l);
        out->pins[n_in + j][l] = '\0';
        out->pdir[n_in + j] = 2;
    }

    n_combo = 1u << n_in;
    for (combo = 0; combo < n_combo; combo++) {
        cd_row_t *row = &out->rows[combo];

        for (j = 0; j < n_in; j++)
            row->ins[j] = (int8_t)((combo >> j) & 1u);

        for (j = 0; j < n_out; j++) {
            int8_t v;
            if (lb_frun(rpn[j], rlen[j], row->ins, &v) != 0) return -1;
            row->outs[j] = v;
        }
    }
    out->n_row = (uint16_t)n_combo;

    return 0;
}
