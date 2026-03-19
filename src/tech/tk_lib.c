/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_lib.c -- Liberty .lib parser for Takahe
 *
 * Reads just enough of a Liberty file to map gates:
 * cell names, pin names, area, direction, function strings.
 * Everything else (timing arcs, NLDM tables, power) is
 * politely ignored by counting braces like a compiler
 * ignoring comments — the structure is there, we just
 * don't care about its contents.
 *
 * A 14MB SKY130 .lib parses in one forward pass.
 * No recursion, no alloc beyond the initial file read.
 */

#include "takahe.h"
#include <ctype.h>

/* ---- String interning into lb_lib_t ---- */

static uint32_t
lb_sint(lb_lib_t *lib, const char *s, uint16_t len)
{
    uint32_t off;
    if (lib->str_len + len + 1 > LB_MAX_STRS) return 0;
    off = lib->str_len;
    memcpy(lib->strs + off, s, len);
    lib->strs[off + len] = '\0';
    lib->str_len += len + 1;
    return off;
}

/* ---- Skip whitespace ---- */

static const char *
lb_skip(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' ||
                       *p == '\n' || *p == '\r'))
        p++;
    /* Skip line comments */
    while (p + 1 < end && *p == '/' && *(p + 1) == '*') {
        p += 2;
        while (p + 1 < end && !(*p == '*' && *(p + 1) == '/'))
            p++;
        if (p + 1 < end) p += 2;
        while (p < end && (*p == ' ' || *p == '\t' ||
                           *p == '\n' || *p == '\r'))
            p++;
    }
    return p;
}

/* ---- Extract quoted string: "foo" → foo ---- */

static const char *
lb_qstr(const char *p, const char *end, const char **out,
         uint16_t *olen)
{
    const char *s;
    *out = ""; *olen = 0;
    while (p < end && *p != '"') p++;
    if (p >= end) return p;
    p++; /* skip opening quote */
    s = p;
    while (p < end && *p != '"') p++;
    *out = s;
    *olen = (uint16_t)(p - s);
    if (p < end) p++; /* skip closing quote */
    return p;
}

/* ---- Extract parenthesised name: (foo) → foo ---- */

static const char *
lb_pnam(const char *p, const char *end, const char **out,
         uint16_t *olen)
{
    const char *s;
    *out = ""; *olen = 0;
    while (p < end && *p != '(') p++;
    if (p >= end) return p;
    p++; /* skip ( */
    /* skip whitespace and quote */
    while (p < end && (*p == ' ' || *p == '"')) p++;
    s = p;
    while (p < end && *p != ')' && *p != '"' && *p != ',') p++;
    *out = s;
    *olen = (uint16_t)(p - s);
    /* trim trailing space */
    while (*olen > 0 && ((*out)[*olen - 1] == ' ' ||
                          (*out)[*olen - 1] == '\t'))
        (*olen)--;
    while (p < end && *p != ')') p++;
    if (p < end) p++;
    return p;
}

/* ---- Skip to matching close brace ---- */

static const char *
lb_sbrc(const char *p, const char *end)
{
    int depth = 1;
    KA_GUARD(g, 100000000);
    while (p < end && depth > 0 && g--) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        if (depth > 0) p++;
    }
    if (p < end) p++; /* skip final } */
    return p;
}

/* ---- Match keyword at position ---- */

static int
lb_kw(const char *p, const char *end, const char *kw, int klen)
{
    if (p + klen > end) return 0;
    if (memcmp(p, kw, (size_t)klen) != 0) return 0;
    /* Must be followed by space/tab/( — not part of longer word */
    if (p + klen < end) {
        char c = p[klen];
        if (c != ' ' && c != '\t' && c != '(' && c != '\n' &&
            c != '\r' && c != ':')
            return 0;
    }
    return 1;
}

/* ---- Extract value after colon: key : "value" ; ---- */

static const char *
lb_cval(const char *p, const char *end, const char **out,
         uint16_t *olen)
{
    *out = ""; *olen = 0;
    /* Find : */
    while (p < end && *p != ':') p++;
    if (p >= end) return p;
    p++; /* skip : */
    p = lb_skip(p, end);

    if (p < end && *p == '"') {
        p = lb_qstr(p, end, out, olen);
    } else {
        /* Unquoted value */
        const char *s = p;
        while (p < end && *p != ';' && *p != '\n') p++;
        *out = s;
        *olen = (uint16_t)(p - s);
        while (*olen > 0 && ((*out)[*olen - 1] == ' ' ||
                              (*out)[*olen - 1] == '\t'))
            (*olen)--;
    }
    /* Skip to ; */
    while (p < end && *p != ';') p++;
    if (p < end) p++;
    return p;
}

/* ---- Parse comma/space-separated floats from raw text ---- */

static int
lb_pfv(const char *s, const char *end, int64_t *out, int max,
       double scale)
{
    int n = 0;
    const char *p = s;
    while (p < end && n < max) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '"' ||
               *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t'))
            p++;
        if (p >= end || *p == ')') break;
        {
            char *ep;
            double v = strtod(p, &ep);
            if (ep == p) break;
            out[n++] = (int64_t)(v * scale + 0.5);
            p = ep;
        }
    }
    return n;
}

/* ---- Extract one NLDM table from raw cell body text ----
 * Scans for the given keyword (e.g. "cell_rise") and parses
 * the index_1, index_2, values arrays into an lb_nldm_t.
 * All values converted to femtoseconds (delays/slew) or
 * attofarads (capacitance) at parse time. After this,
 * no more floats. */

static void
lb_xtbl(const char *body, const char *end, const char *kw,
        int kwlen, lb_nldm_t *tbl, double sc1, double sc2,
        double scv)
{
    const char *p = body;
    int depth = 0;
    KA_GUARD(gx, 1000000);

    memset(tbl, 0, sizeof(*tbl));

    while (p < end && gx--) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { depth--; p++; continue; }

        /* Find the keyword */
        if (p + kwlen < end && memcmp(p, kw, (size_t)kwlen) == 0) {
            /* Found — now scan for index_1, index_2, values
             * within this block (until matching }) */
            int bdepth = 0;
            p += kwlen;
            while (p < end && *p != '{') p++;
            if (p < end) { p++; bdepth = 1; }

            KA_GUARD(gi, 100000);
            while (p < end && bdepth > 0 && gi--) {
                if (*p == '{') { bdepth++; p++; continue; }
                if (*p == '}') { bdepth--; p++; continue; }

                if (p + 7 < end && memcmp(p, "index_1", 7) == 0) {
                    /* Skip to the quoted string */
                    while (p < end && *p != '"') p++;
                    if (p < end) {
                        p++;
                        tbl->n1 = (uint8_t)lb_pfv(p, end, tbl->idx1,
                                                   LB_NLDM_SZ, sc1);
                        while (p < end && *p != ';') p++;
                    }
                } else if (p + 7 < end && memcmp(p, "index_2", 7) == 0) {
                    while (p < end && *p != '"') p++;
                    if (p < end) {
                        p++;
                        tbl->n2 = (uint8_t)lb_pfv(p, end, tbl->idx2,
                                                   LB_NLDM_SZ, sc2);
                        while (p < end && *p != ';') p++;
                    }
                } else if (p + 6 < end && memcmp(p, "values", 6) == 0) {
                    while (p < end && *p != '(') p++;
                    if (p < end) {
                        const char *vs;
                        int vdp = 1;
                        p++;
                        vs = p;
                        KA_GUARD(gvp, 100000);
                        while (p < end && vdp > 0 && gvp--) {
                            if (*p == '(') vdp++;
                            else if (*p == ')') vdp--;
                            if (vdp > 0) p++;
                        }
                        lb_pfv(vs, p, tbl->vals,
                               LB_NLDM_SZ * LB_NLDM_SZ, scv);
                    }
                }
                p++;
            }

            if (tbl->n1 > 0 && tbl->n2 > 0) tbl->valid = 1;
            return;  /* found and parsed, done */
        }
        p++;
    }
}

/* ---- NLDM lookup via PCHIP ---- */

tk_fs_t
lb_dly(const lb_nldm_t *tbl, tk_fs_t slew, tk_af_t load)
{
    if (!tbl || !tbl->valid) return 0;
    return pc_lk2d(tbl->idx1, (int)tbl->n1,
                   tbl->idx2, (int)tbl->n2,
                   tbl->vals, slew, load);
}

/* ---- Extract worst-case delay from a cell body ----
 * Scans the raw text for values(...) blocks and finds the
 * maximum float. Quick and dirty but correct — the max
 * value in any NLDM table is the worst-case delay. */

/* ---- Find max value from cell_rise/cell_fall tables only ----
 * Skips transition, constraint, and power tables that would
 * inflate the worst-case delay with unrelated numbers. Like
 * measuring a building's height but accidentally including
 * the depth of the car park underneath. */

static tk_fs_t
lb_wcdly(const char *body, const char *end)
{
    const char *p = body;
    double maxv = 0.0;
    int in_dly = 0;  /* 1 = inside cell_rise or cell_fall block */
    int depth = 0;
    KA_GUARD(gwc, 1000000);

    while (p < end && gwc--) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') {
            if (in_dly && depth <= 1) in_dly = 0;
            depth--;
            p++;
            continue;
        }

        /* Enter cell_rise or cell_fall blocks */
        if (p + 9 < end && (memcmp(p, "cell_rise", 9) == 0 ||
                             memcmp(p, "cell_fall", 9) == 0)) {
            in_dly = 1;
            p += 9;
            continue;
        }

        /* Exit on transition/constraint tables */
        if (p + 10 < end && (memcmp(p, "rise_trans", 10) == 0 ||
                              memcmp(p, "fall_trans", 10) == 0)) {
            in_dly = 0;
        }

        /* Only scan values inside cell_rise/cell_fall */
        if (in_dly && p + 6 < end && memcmp(p, "values", 6) == 0) {
            p += 6;
            while (p < end && *p != '(') p++;
            if (p < end) p++;
            while (p < end && *p != ')') {
                while (p < end && (*p == ' ' || *p == ',' ||
                       *p == '"' || *p == '\\' || *p == '\n' ||
                       *p == '\r' || *p == '\t'))
                    p++;
                if (p < end && *p != ')') {
                    char *ep;
                    double v = strtod(p, &ep);
                    if (ep > p && v > maxv) maxv = v;
                    p = ep > p ? ep : p + 1;
                }
            }
        }
        p++;
    }
    return TK_NS2FS(maxv);
}

/* ---- Public: load Liberty .lib file ---- */

int
lb_load(lb_lib_t *lib, const char *path)
{
    FILE *fp;
    char *buf;
    long fsz;
    size_t nr;
    const char *p, *end;

    if (!lib || !path) return -1;
    memset(lib, 0, sizeof(*lib));
    lib->str_len = 1; /* sentinel */

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "takahe: cannot open lib '%s'\n", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsz <= 0 || fsz > 32 * 1024 * 1024) {
        fprintf(stderr, "takahe: lib file too large (%ld)\n", fsz);
        fclose(fp);
        return -1;
    }

    buf = (char *)malloc((size_t)fsz + 1);
    if (!buf) { fclose(fp); return -1; }
    nr = fread(buf, 1, (size_t)fsz, fp);
    fclose(fp);
    buf[nr] = '\0';

    p = buf;
    end = buf + nr;

    /* Scan for cell blocks */
    KA_GUARD(gcell, 2000000);
    while (p < end && gcell--) {
        p = lb_skip(p, end);
        if (p >= end) break;

        /* Look for "cell" keyword */
        if (lb_kw(p, end, "cell", 4)) {
            const char *cname; uint16_t cnlen;
            lb_cell_t *cell;
            int depth;

            p += 4;
            p = lb_pnam(p, end, &cname, &cnlen);
            p = lb_skip(p, end);
            if (p >= end || *p != '{') continue;
            p++; /* skip { */

            if (lib->n_cell >= LB_MAX_CELLS) {
                /* Skip this cell entirely */
                p = lb_sbrc(p, end);
                continue;
            }

            cell = &lib->cells[lib->n_cell];
            memset(cell, 0, sizeof(*cell));
            cell->name_off = lb_sint(lib, cname, cnlen);
            cell->name_len = cnlen;
            cell->kind = LB_COMB;
            cell->clk_pin = 0xFF;
            cell->d_pin = 0xFF;
            cell->q_pin = 0xFF;
            cell->rst_pin = 0xFF;

            /* Parse cell body */
            depth = 1;
            {const char *cell_body = p;
            KA_GUARD(gbody, 10000000);
            while (p < end && depth > 0 && gbody--) {
                p = lb_skip(p, end);
                if (p >= end) break;

                if (*p == '}') { depth--; p++; continue; }

                if (lb_kw(p, end, "area", 4)) {
                    const char *av; uint16_t al;
                    p = lb_cval(p, end, &av, &al);
                    cell->area = (float)strtod(av, NULL);
                } else if (lb_kw(p, end, "pg_pin", 6)) {
                    /* Skip power/ground pins entirely */
                    while (p < end && *p != '{') p++;
                    if (p < end) { p++; p = lb_sbrc(p, end); }
                } else if (lb_kw(p, end, "pin", 3) &&
                           cell->n_pin < LB_MAX_PINS) {
                    /* Signal pin */
                    uint8_t pidx = cell->n_pin;
                    const char *pnm; uint16_t pnl;
                    const char *save_p = p + 3;

                    /* Extract pin name before parsing body */
                    {
                        const char *tp = save_p;
                        while (tp < end && *tp != '(') tp++;
                        if (tp < end) {
                            tp++; /* skip ( */
                            while (tp < end && (*tp==' '||*tp=='"')) tp++;
                            pnm = tp;
                            while (tp < end && *tp!=')' && *tp!='"') tp++;
                            pnl = (uint16_t)(tp - pnm);
                        } else {
                            pnm = ""; pnl = 0;
                        }
                    }

                    /* Now parse the pin block for direction/function */
                    p += 3;
                    {
                        const char *ppname; uint16_t ppnl;
                        p = lb_pnam(p, end, &ppname, &ppnl);
                        p = lb_skip(p, end);
                        if (p < end && *p == '{') {
                            int pdepth = 1;
                            const char *pdir = NULL; uint16_t pdl = 0;
                            const char *pfn = NULL; uint16_t pfl = 0;
                            int pclk = 0;
                            p++;

                            KA_GUARD(gpin, 1000000);
                            while (p < end && pdepth > 0 && gpin--) {
                                p = lb_skip(p, end);
                                if (p >= end) break;
                                if (*p == '{') { pdepth++; p++; continue; }
                                if (*p == '}') { pdepth--; p++; continue; }

                                if (lb_kw(p, end, "direction", 9)) {
                                    p = lb_cval(p, end, &pdir, &pdl);
                                } else if (lb_kw(p, end, "function", 8)) {
                                    p = lb_cval(p, end, &pfn, &pfl);
                                } else if (lb_kw(p, end, "clock", 5)) {
                                    const char *cv2; uint16_t cl2;
                                    p = lb_cval(p, end, &cv2, &cl2);
                                    if (cl2 >= 4 && memcmp(cv2, "true", 4) == 0)
                                        pclk = 1;
                                } else if (lb_kw(p, end, "capacitance", 11)) {
                                    const char *cv3; uint16_t cl3;
                                    p = lb_cval(p, end, &cv3, &cl3);
                                    if (cell->pins[pidx].cap == 0)
                                        cell->pins[pidx].cap =
                                            TK_PF2AF(strtod(cv3, NULL));
                                } else {
                                    while (p < end && *p != ';' && *p != '{' && *p != '}')
                                        p++;
                                    if (p < end && *p == ';') p++;
                                }
                            }

                            /* Store pin */
                            cell->pins[pidx].name_off = lb_sint(lib, pnm, pnl);
                            cell->pins[pidx].name_len = pnl;
                            cell->pins[pidx].dir = LB_DIR_IN;
                            if (pdir && pdl >= 6 && memcmp(pdir, "output", 6) == 0)
                                cell->pins[pidx].dir = LB_DIR_OUT;
                            cell->pins[pidx].is_clk = (uint8_t)pclk;
                            cell->pins[pidx].func_off = 0;
                            cell->pins[pidx].func_len = 0;
                            if (pfn && pfl > 0) {
                                cell->pins[pidx].func_off = lb_sint(lib, pfn, pfl);
                                cell->pins[pidx].func_len = pfl;
                            }

                            cell->n_pin++;
                            if (cell->pins[pidx].dir == LB_DIR_IN)
                                cell->n_in++;
                        }
                    }
                } else if (lb_kw(p, end, "ff", 2)) {
                    cell->kind = LB_DFF;
                    /* Skip to { then skip block, but note clocked_on/next_state */
                    while (p < end && *p != '{') p++;
                    if (p < end) {
                        int fdepth = 1;
                        const char *clkon = NULL; uint16_t cll = 0;
                        const char *nxst = NULL; uint16_t nxl = 0;
                        const char *clr = NULL; uint16_t crl = 0;
                        p++;
                        KA_GUARD(gff, 10000);
                        while (p < end && fdepth > 0 && gff--) {
                            p = lb_skip(p, end);
                            if (p >= end) break;
                            if (*p == '{') { fdepth++; p++; continue; }
                            if (*p == '}') { fdepth--; p++; continue; }
                            if (lb_kw(p, end, "clocked_on", 10))
                                p = lb_cval(p, end, &clkon, &cll);
                            else if (lb_kw(p, end, "next_state", 10))
                                p = lb_cval(p, end, &nxst, &nxl);
                            else if (lb_kw(p, end, "clear", 5))
                                p = lb_cval(p, end, &clr, &crl);
                            else {
                                while (p < end && *p != ';' && *p != '{' && *p != '}') p++;
                                if (p < end && *p == ';') p++;
                            }
                        }
                        (void)clkon; (void)nxst; (void)clr;
                    }
                } else if (lb_kw(p, end, "latch", 5)) {
                    cell->kind = LB_DLAT;
                    while (p < end && *p != '{') p++;
                    if (p < end) { p++; p = lb_sbrc(p, end); }
                } else if (*p == '{') {
                    /* Unknown group — skip */
                    p++; p = lb_sbrc(p, end);
                } else {
                    /* Skip line */
                    while (p < end && *p != ';' && *p != '{' && *p != '}')
                        p++;
                    if (p < end && *p == ';') p++;
                }
            }

            /* Post-process: identify CLK/D/Q/RST pins by name */
            {
                uint8_t j;
                for (j = 0; j < cell->n_pin && j < LB_MAX_PINS; j++) {
                    const char *pn = lib->strs + cell->pins[j].name_off;
                    uint16_t pl = cell->pins[j].name_len;
                    if (pl == 3 && memcmp(pn, "CLK", 3) == 0)
                        cell->clk_pin = j;
                    else if (pl == 1 && *pn == 'D')
                        cell->d_pin = j;
                    else if (pl == 1 && *pn == 'Q')
                        cell->q_pin = j;
                    else if (pl == 7 && memcmp(pn, "RESET_B", 7) == 0)
                        cell->rst_pin = j;
                }
            }

            /* Check for tie cell: output with function "0" or "1" */
            {
                uint8_t j;
                for (j = 0; j < cell->n_pin && j < LB_MAX_PINS; j++) {
                    if (cell->pins[j].dir == LB_DIR_OUT &&
                        cell->pins[j].func_len > 0) {
                        const char *fn = lib->strs + cell->pins[j].func_off;
                        if (cell->pins[j].func_len == 1 &&
                            (*fn == '0' || *fn == '1'))
                            cell->kind = LB_TIE;
                    }
                }
            }

            /* Extract worst-case delay from NLDM values blocks */
            cell->delay_max = lb_wcdly(cell_body, p);

            /* Extract full NLDM tables for output pins.
             * index_1 = input slew (ns → fs: ×10^6)
             * index_2 = output load (pF → aF: ×10^6)
             * values  = delay (ns → fs: ×10^6) */
            {
                uint8_t pi;
                for (pi = 0; pi < cell->n_pin; pi++) {
                    if (cell->pins[pi].dir == LB_DIR_OUT) {
                        lb_xtbl(cell_body, p, "cell_rise", 9,
                                &cell->pins[pi].rise,
                                1e6, 1e6, 1e6);
                        lb_xtbl(cell_body, p, "cell_fall", 9,
                                &cell->pins[pi].fall,
                                1e6, 1e6, 1e6);
                    }
                }
            }
            } /* close cell_body scope */
            lib->n_cell++;
        } else {
            /* Not a cell keyword — advance one character */
            p++;
        }
    }

    free(buf);
    printf("takahe: lib: %u cells from %s\n", lib->n_cell, path);
    return 0;
}
