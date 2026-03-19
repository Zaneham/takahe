/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_cdef.c -- Cell definition loader for Takahe
 *
 * Reads .def files that describe logic cells as truth tables.
 * Binary AND, ternary min, stochastic multiply — all just
 * rows in a table. The engine doesn't know or care what
 * number system you believe in.
 *
 * Brusentsov's Setun, Shannon's relays, and von Neumann's
 * probabilistic automata all reduce to the same thing:
 * a function from input digits to output digits. That's
 * what this file loads.
 *
 * Format:
 *   cell <NAME> radix <N> [stochastic] inputs <N> outputs <N>
 *   pin <name> <in|out> [function-string]
 *   truth <v0> <v1> ... -> <v0> ...
 *   end
 */

#include "takahe.h"
#include <ctype.h>

#define CD_MAXLN 512  /* max line length */

/* ---- Skip whitespace ---- */

static const char *
cd_skip(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* ---- Extract word, return pointer past it ---- */

static const char *
cd_word(const char *p, char *buf, int bsz)
{
    int i = 0;
    p = cd_skip(p);
    while (*p && *p != ' ' && *p != '\t' &&
           *p != '\n' && *p != '\r' && *p != '#') {
        if (i < bsz - 1) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return p;
}

/* ---- Parse integer (handles negative for ternary) ---- */

static const char *
cd_int(const char *p, int8_t *out)
{
    int neg = 0;
    int val = 0;
    p = cd_skip(p);
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    *out = (int8_t)(neg ? -val : val);
    return p;
}

/* ---- Public: load cell definitions from .def file ---- */

int
cd_load(cd_lib_t *lib, const char *path)
{
    FILE *fp;
    char line[CD_MAXLN];
    cd_cell_t *cur = NULL;

    if (!lib || !path) return -1;
    memset(lib, 0, sizeof(*lib));

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "takahe: cannot open cell def '%s'\n", path);
        return -1;
    }

    KA_GUARD(gln, 100000);
    while (fgets(line, CD_MAXLN, fp) && gln--) {
        const char *p = cd_skip(line);
        char kw[32];

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        p = cd_word(p, kw, 32);

        if (strcmp(kw, "cell") == 0) {
            /* cell <NAME> radix <N> [stochastic] inputs <N> outputs <N> */
            char name[32], tok[32];

            if (lib->n_cell >= CD_MAX_CELLS) { cur = NULL; continue; }
            cur = &lib->cells[lib->n_cell];
            memset(cur, 0, sizeof(*cur));

            p = cd_word(p, name, 32);
            memcpy(cur->name, name, 31);
            cur->name[31] = '\0';

            /* Parse remaining key-value pairs on this line */
            KA_GUARD(gkv, 20);
            while (*p && *p != '\n' && *p != '#' && gkv--) {
                p = cd_word(p, tok, 32);
                if (tok[0] == '\0') break;

                if (strcmp(tok, "radix") == 0) {
                    int8_t v; p = cd_int(p, &v);
                    cur->radix = (uint8_t)v;
                } else if (strcmp(tok, "stochastic") == 0) {
                    cur->stoch = 1;
                } else if (strcmp(tok, "inputs") == 0) {
                    int8_t v; p = cd_int(p, &v);
                    cur->n_in = (uint8_t)v;
                } else if (strcmp(tok, "outputs") == 0) {
                    int8_t v; p = cd_int(p, &v);
                    cur->n_out = (uint8_t)v;
                }
            }
        } else if (strcmp(kw, "pin") == 0 && cur) {
            /* pin <name> <in|out> [function] */
            char pname[16], pdir[8];
            uint8_t pi = cur->n_pin;
            if (pi >= CD_MAX_PINS) continue;

            p = cd_word(p, pname, 16);
            p = cd_word(p, pdir, 8);

            memcpy(cur->pins[pi], pname, 15);
            cur->pins[pi][15] = '\0';
            cur->pdir[pi] = (strcmp(pdir, "out") == 0) ? 2 : 1;
            cur->n_pin++;

            /* Rest of line is function string (optional) */
            p = cd_skip(p);
            if (*p && *p != '\n' && *p != '#') {
                int fi = 0;
                while (*p && *p != '\n' && *p != '#' && fi < 63)
                    cur->func[fi++] = *p++;
                cur->func[fi] = '\0';
            }
        } else if (strcmp(kw, "truth") == 0 && cur) {
            /* truth <in0> <in1> ... -> <out0> ... */
            cd_row_t *row;
            int vi = 0;
            int phase = 0;  /* 0=inputs, 1=outputs */
            char tok2[16];

            if (cur->n_row >= CD_MAX_ROWS) continue;
            row = &cur->rows[cur->n_row];
            memset(row, 0, sizeof(*row));

            KA_GUARD(gv, 32);
            while (gv--) {
                p = cd_word(p, tok2, 16);
                if (tok2[0] == '\0') break;

                if (strcmp(tok2, "->") == 0) {
                    phase = 1;
                    vi = 0;
                    continue;
                }

                if (phase == 0 && vi < CD_MAX_VALS) {
                    /* Parse as integer — handles -1, 0, 1 */
                    int neg2 = 0, val2 = 0;
                    const char *t = tok2;
                    if (*t == '-') { neg2 = 1; t++; }
                    while (*t >= '0' && *t <= '9')
                        val2 = val2 * 10 + (*t++ - '0');
                    row->ins[vi++] = (int8_t)(neg2 ? -val2 : val2);
                } else if (phase == 1 && vi < CD_MAX_VALS) {
                    int neg2 = 0, val2 = 0;
                    const char *t = tok2;
                    if (*t == '-') { neg2 = 1; t++; }
                    while (*t >= '0' && *t <= '9')
                        val2 = val2 * 10 + (*t++ - '0');
                    row->outs[vi++] = (int8_t)(neg2 ? -val2 : val2);
                }
            }

            cur->n_row++;
        } else if (strcmp(kw, "end") == 0 && cur) {
            lib->n_cell++;
            cur = NULL;
        }
    }

    fclose(fp);
    printf("takahe: cell defs: %u cells from %s\n",
           lib->n_cell, path);
    return 0;
}

/* ---- Find cell by name and radix ----
 * Prefers non-stochastic cells when radix matches.
 * Stochastic AND (radix=2, stoch=1) and binary AND (radix=2)
 * share the same truth table but different semantics.
 * The optimiser wants binary; the stochastic analyser wants
 * stochastic. Default: return non-stochastic first. */

const cd_cell_t *
cd_find(const cd_lib_t *lib, const char *name, uint8_t radix)
{
    uint32_t i;
    const cd_cell_t *stoch_f = NULL;
    if (!lib || !name) return NULL;
    for (i = 0; i < lib->n_cell; i++) {
        if (lib->cells[i].radix == radix &&
            strcmp(lib->cells[i].name, name) == 0) {
            if (!lib->cells[i].stoch) return &lib->cells[i];
            if (!stoch_f) stoch_f = &lib->cells[i];
        }
    }
    return stoch_f;  /* stochastic fallback if no normal match */
}

/* ---- Evaluate truth table for given inputs ---- */

int8_t
cd_eval(const cd_cell_t *cell, const int8_t *ins, int8_t *outs)
{
    uint16_t r;
    uint8_t j;

    if (!cell || !ins || !outs) return -1;

    for (r = 0; r < cell->n_row; r++) {
        const cd_row_t *row = &cell->rows[r];
        int match = 1;
        for (j = 0; j < cell->n_in; j++) {
            if (row->ins[j] != ins[j]) { match = 0; break; }
        }
        if (match) {
            for (j = 0; j < cell->n_out; j++)
                outs[j] = row->outs[j];
            return 0;
        }
    }
    return -1;  /* no matching row — undefined input */
}
