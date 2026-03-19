/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_vlog.c -- Gate-level structural Verilog emitter for Takahe
 *
 * The final act: RTL becomes real cells with real names.
 * Output goes straight to OpenROAD. No Yosys in between.
 * If this emitter does its job, the netlist is clean enough
 * for P&R without any post-processing.
 *
 * Fixes from the OpenROAD debugging marathon:
 *   1. No empty wire names (skip nets with len=0)
 *   2. Verilog reserved words escaped with backslash
 *   3. CONST cells emit as assign, not tie cells (avoids
 *      POWER-type nets that block TritonRoute)
 *   4. Bus ports use [N] indexing in cell connections
 *   5. Module name from first MODULE node, not hardcoded
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Verilog reserved words ----
 * If a net name matches one of these, prefix with \ and
 * suffix with space. Verilog's escaped identifier syntax. */

static const char *vlog_rsvd[] = {
    "always", "and", "assign", "begin", "buf", "case",
    "const", "default", "end", "endcase", "endmodule",
    "for", "function", "generate", "if", "initial",
    "input", "integer", "module", "nand", "nor", "not",
    "or", "output", "parameter", "reg", "supply0",
    "supply1", "task", "time", "tri", "wire", "xnor",
    "xor", "super", NULL
};

static int
em_isrsv(const char *nm, uint16_t len)
{
    int i;
    for (i = 0; vlog_rsvd[i]; i++) {
        uint16_t rl = (uint16_t)strlen(vlog_rsvd[i]);
        if (len == rl && memcmp(nm, vlog_rsvd[i], len) == 0)
            return 1;
    }
    return 0;
}

/* ---- Net name helper ----
 * Handles: empty names, reserved words, bus indexing. */

static const char *
em_nnam(const rt_mod_t *M, uint32_t ni, char *buf, int bsz)
{
    const rt_net_t *n;
    const char *nm;
    uint16_t nl;

    if (ni == 0 || ni >= M->n_net) {
        snprintf(buf, (size_t)bsz, "1'b0");
        return buf;
    }
    n = &M->nets[ni];
    nm = M->strs + n->name_off;
    nl = n->name_len;

    /* Skip empty names */
    if (nl == 0) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "_n%u", ni);
        snprintf(buf, (size_t)bsz, "%s", tmp);
        return buf;
    }

    /* Internal nets: append net index to avoid collisions.
     * Multiple RTL nets can share a base name (e.g. "cmx" from
     * each MUX in a case chain, or "tmp" from each op). Ports
     * keep their clean names since they're unique by definition.
     *
     * Do this BEFORE reserved word check — "const_6" is not
     * reserved even though "const" is. */
    if (n->is_port == 0) {
        snprintf(buf, (size_t)bsz, "%.*s_%u", (int)nl, nm, ni);
    } else {
        /* Check for duplicate port names (multi-module designs).
         * If another port has the same name, disambiguate. */
        int dup = 0;
        {
            uint32_t k;
            for (k = 1; k < M->n_net; k++) {
                if (k == ni) continue;
                if (M->nets[k].is_port != 0 &&
                    M->nets[k].name_len == nl &&
                    memcmp(M->strs + M->nets[k].name_off, nm, nl) == 0) {
                    dup = 1; break;
                }
            }
        }
        if (dup) {
            snprintf(buf, (size_t)bsz, "%.*s_%u", (int)nl, nm, ni);
        } else {
            snprintf(buf, (size_t)bsz, "%.*s", (int)nl, nm);
            if (em_isrsv(nm, nl))
                snprintf(buf, (size_t)bsz, "\\%.*s ", (int)nl, nm);
        }
    }
    return buf;
}

/* ---- Check if a net is part of a bus port ----
 * Returns the base name length and bit index, or -1. */

static int
em_busbit(const rt_mod_t *M, uint32_t ni, char *base,
          int bsz, uint32_t *bit)
{
    const rt_net_t *n = &M->nets[ni];
    const char *nm = M->strs + n->name_off;
    int p, blen;

    if (n->name_len == 0) return -1;

    /* Find last underscore */
    for (p = n->name_len - 1; p > 0; p--) {
        if (nm[p] == '_') break;
    }
    if (p <= 0 || p >= n->name_len - 1) return -1;

    /* Check that everything after _ is a digit */
    {
        int q;
        for (q = p + 1; q < n->name_len; q++) {
            if (nm[q] < '0' || nm[q] > '9') return -1;
        }
    }

    blen = p;
    if (blen >= bsz) blen = bsz - 1;
    memcpy(base, nm, (size_t)blen);
    base[blen] = '\0';
    *bit = (uint32_t)atoi(nm + p + 1);

    /* Verify the base is actually a UNIQUE port.
     * If multiple ports share the same name (multi-module),
     * don't reconstruct — it would create multi-drivers. */
    {
        uint32_t k;
        int matches = 0;
        for (k = 1; k < M->n_net; k++) {
            const rt_net_t *pk = &M->nets[k];
            if (pk->is_port != 0 && pk->width > 1 &&
                pk->name_len == (uint16_t)blen &&
                memcmp(M->strs + pk->name_off, base, (size_t)blen) == 0)
                matches++;
        }
        if (matches != 1) return -1; /* ambiguous or not found */
        for (k = 1; k < M->n_net; k++) {
            const rt_net_t *pk = &M->nets[k];
            if (pk->is_port != 0 && pk->width > 1 &&
                pk->name_len == (uint16_t)blen &&
                memcmp(M->strs + pk->name_off, base, (size_t)blen) == 0)
                return blen;
        }
    }
    return -1;
}

/* ---- Emit a net reference in a cell connection ----
 * For bus port bits: emit base[N] instead of base_N */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static const char *
em_cnet(const rt_mod_t *M, uint32_t ni, char *buf, int bsz)
{
    char base[64];
    uint32_t bit;

    if (em_busbit(M, ni, base, 48, &bit) >= 0) {
        snprintf(buf, (size_t)bsz, "%s[%u]", base, bit);
        return buf;
    }
    return em_nnam(M, ni, buf, bsz);
}
#pragma GCC diagnostic pop

/* ---- Cell input: inline constants ----
 * If net ni is driven by a CONST cell, return "1'b0"/"1'b1".
 * Otherwise return the normal wire name via em_cnet.
 * This eliminates CONST wires entirely — no assign, no tie
 * cell, no POWER/GROUND net for TritonRoute to choke on. */

static const char *
em_cin(const rt_mod_t *M, uint32_t ni, char *buf, int bsz)
{
    uint32_t drv;
    if (ni > 0 && ni < M->n_net) {
        drv = M->nets[ni].driver;
        if (drv > 0 && drv < M->n_cell &&
            M->cells[drv].type == RT_CONST) {
            snprintf(buf, (size_t)bsz, "1'b%d",
                     M->cells[drv].param ? 1 : 0);
            return buf;
        }
    }
    return em_cnet(M, ni, buf, bsz);
}

/* ---- Find output pin name for a cell type ---- */

static const char *
em_opin(const lb_lib_t *lib, const lb_cell_t *lc)
{
    uint8_t j;
    for (j = 0; j < lc->n_pin && j < LB_MAX_PINS; j++) {
        if (lc->pins[j].dir == LB_DIR_OUT)
            return lib->strs + lc->pins[j].name_off;
    }
    return "Y";
}

/* ---- Find the design's module name ---- */

static const char *
em_mname(const rt_mod_t *M)
{
    if (M->mod_name[0] != '\0')
        return M->mod_name;
    return "takahe_top";
}

/* ---- Public: emit gate-level structural Verilog ---- */

int
em_vlog(const rt_mod_t *M, const lb_lib_t *lib,
        const mp_bind_t *tbl, FILE *fp)
{
    uint32_t i;
    uint32_t ucnt = 0;
    char nb[64], n0[64], n1[64], n2[64];

    if (!M || !lib || !tbl || !fp) return -1;

    fprintf(fp, "// Generated by Takahe v%d.%d.%d\n",
            TK_VER_MAJOR, TK_VER_MINOR, TK_VER_PATCH);
    fprintf(fp, "// Target: sky130_fd_sc_hd\n\n");

    fprintf(fp, "module %s (\n", em_mname(M));

    /* ---- Port declarations with bus reconstruction ---- */
    {
        int first = 1;
        uint8_t *done = (uint8_t *)calloc(M->n_net, 1);
        if (!done) { fprintf(fp, ");\n\n"); goto wires; }

        for (i = 1; i < M->n_net; i++) {
            const rt_net_t *n = &M->nets[i];
            const char *nm;
            uint16_t nl;
            uint32_t k, hi;
            const char *dir;
            char base[64];
            int blen;

            if (n->is_port == 0) continue;
            if (done[i]) continue;

            nm = M->strs + n->name_off;
            nl = n->name_len;

            /* Skip empty-named nets */
            if (nl == 0) { done[i] = 1; continue; }

            /* Skip original multi-bit net if slices exist */
            if (n->width > 1) {
                char probe[64];
                int pl = snprintf(probe, 64, "%.*s_0", (int)nl, nm);
                uint32_t found = 0;
                for (k = 1; k < M->n_net; k++) {
                    const rt_net_t *pk = &M->nets[k];
                    if (pk->name_len == (uint16_t)pl &&
                        memcmp(M->strs + pk->name_off, probe, (size_t)pl) == 0 &&
                        pk->is_port == n->is_port) {
                        found = 1; break;
                    }
                }
                if (found) { done[i] = 1; continue; }
            }

            /* Check if this is a _0 slice — reconstruct bus */
            {
                const char *us = NULL;
                int p2;
                for (p2 = nl - 1; p2 > 0; p2--) {
                    if (nm[p2] == '_') { us = nm + p2; break; }
                }
                if (us && us[1] == '0' && (us[2] == '\0' || p2 + 2 >= (int)nl)) {
                    blen = (int)(us - nm);
                    if (blen > 0 && blen < 60) {
                        memcpy(base, nm, (size_t)blen);
                        base[blen] = '\0';
                        hi = 0;
                        for (k = 1; k < M->n_net; k++) {
                            const rt_net_t *pk = &M->nets[k];
                            if (pk->is_port == n->is_port &&
                                pk->name_len > (uint16_t)blen + 1 &&
                                memcmp(M->strs + pk->name_off, base, (size_t)blen) == 0 &&
                                (M->strs + pk->name_off)[blen] == '_') {
                                uint32_t bit = (uint32_t)atoi(M->strs + pk->name_off + blen + 1);
                                if (bit > hi) hi = bit;
                                done[k] = 1;
                            }
                        }
                        done[i] = 1;
                        dir = n->is_port == 1 ? "input " :
                              n->is_port == 2 ? "output" : "inout ";
                        if (!first) fprintf(fp, ",\n");
                        first = 0;
                        if (hi > 0)
                            fprintf(fp, "    %s [%u:0] %s", dir, hi, base);
                        else
                            fprintf(fp, "    %s %s", dir, base);
                        continue;
                    }
                }
            }

            /* Scalar port */
            dir = n->is_port == 1 ? "input " :
                  n->is_port == 2 ? "output" : "inout ";
            if (!first) fprintf(fp, ",\n");
            first = 0;
            fprintf(fp, "    %s %s", dir, em_nnam(M, i, nb, 64));
        }
        fprintf(fp, "\n);\n\n");
        free(done);
    }

wires:
    /* ---- Wire declarations ---- */
    {
        uint8_t *wdone = (uint8_t *)calloc(M->n_net, 1);
        for (i = 1; i < M->n_net; i++) {
            const rt_net_t *wn = &M->nets[i];
            const char *wnm;
            uint32_t k2;

            if (wn->is_port != 0) continue;
            if (wn->name_len == 0) continue;
            if (wdone && wdone[i]) continue;
            /* Skip CONST-driven nets — inlined at consumers */
            if (wn->driver > 0 && wn->driver < M->n_cell &&
                M->cells[wn->driver].type == RT_CONST) continue;

            /* Skip multi-bit nets if slices exist */
            if (wn->width > 1) {
                char probe2[64];
                int pl2 = snprintf(probe2, 64, "%.*s_0",
                    (int)wn->name_len, M->strs + wn->name_off);
                int found2 = 0;
                for (k2 = 1; k2 < M->n_net; k2++) {
                    if (M->nets[k2].name_len == (uint16_t)pl2 &&
                        memcmp(M->strs + M->nets[k2].name_off,
                               probe2, (size_t)pl2) == 0) {
                        found2 = 1; break;
                    }
                }
                if (found2) { if (wdone) wdone[i] = 1; continue; }
            }

            wnm = em_nnam(M, i, nb, 64);

            /* Deduplicate */
            if (wdone) {
                for (k2 = i; k2 < M->n_net; k2++) {
                    if (M->nets[k2].name_len == wn->name_len &&
                        memcmp(M->strs + M->nets[k2].name_off,
                               M->strs + wn->name_off,
                               wn->name_len) == 0)
                        wdone[k2] = 1;
                }
            }

            fprintf(fp, "wire %s;\n", wnm);
        }
        free(wdone);
    }
    fprintf(fp, "\n");

    /* ---- Cell instances ---- */
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        const lb_cell_t *lc;
        const char *cname;
        rt_ctype_t ct;

        if (c->type == RT_CELL_COUNT) continue;
        ct = c->type;

        /* Skip CONST cells — inlined at consumers */
        if (ct == RT_CONST) continue;

        /* Look up mapped cell */
        if (!tbl[ct].valid) {
            fprintf(fp, "// UNMAPPED: %s w=%u\n",
                    rt_cname(ct), c->width);
            continue;
        }

        lc = &lib->cells[tbl[ct].cell_idx];
        cname = lib->strs + lc->name_off;

        fprintf(fp, "%.*s U%u ( ",
                (int)lc->name_len, cname, ucnt++);

        /* Pin connections — use em_cnet for bus indexing */
        switch ((int)ct) {
        case RT_DFF:
            fprintf(fp, ".D(%s), ",
                    em_cin(M, c->ins[0], n0, 64));
            /* Use actual pin name from Liberty cell */
            {
                uint8_t j;
                const char *clkpin = "CLK";
                for (j = 0; j < lc->n_pin && j < LB_MAX_PINS; j++) {
                    if (lc->pins[j].is_clk) {
                        clkpin = lib->strs + lc->pins[j].name_off;
                        break;
                    }
                }
                fprintf(fp, ".%s(%s), ", clkpin,
                        em_cin(M, c->ins[1], n1, 64));
            }
            fprintf(fp, ".Q(%s)",
                    em_cnet(M, c->out, nb, 64));
            break;

        case RT_DFFR:
            fprintf(fp, ".D(%s), ",
                    em_cin(M, c->ins[0], n0, 64));
            {
                uint8_t j;
                const char *clkpin = "CLK";
                const char *rstpin = "RESET_B";
                for (j = 0; j < lc->n_pin && j < LB_MAX_PINS; j++) {
                    if (lc->pins[j].is_clk)
                        clkpin = lib->strs + lc->pins[j].name_off;
                    if (lc->pins[j].name_len >= 5 &&
                        memcmp(lib->strs + lc->pins[j].name_off,
                               "RESET", 5) == 0)
                        rstpin = lib->strs + lc->pins[j].name_off;
                }
                fprintf(fp, ".%s(%s), ", clkpin,
                        em_cin(M, c->ins[1], n1, 64));
                fprintf(fp, ".%s(%s), ", rstpin,
                        em_cin(M, c->ins[2], n2, 64));
            }
            fprintf(fp, ".Q(%s)",
                    em_cnet(M, c->out, nb, 64));
            break;

        case RT_MUX:
            fprintf(fp, ".A0(%s), ",
                    em_cin(M, c->ins[1], n0, 64));
            fprintf(fp, ".A1(%s), ",
                    em_cin(M, c->ins[2], n1, 64));
            fprintf(fp, ".S(%s), ",
                    em_cin(M, c->ins[0], n2, 64));
            fprintf(fp, ".X(%s)",
                    em_cnet(M, c->out, nb, 64));
            break;

        case RT_NOT:
            fprintf(fp, ".A(%s), .Y(%s)",
                    em_cin(M, c->ins[0], n0, 64),
                    em_cnet(M, c->out, nb, 64));
            break;

        case RT_NAND: case RT_NOR: case RT_XNOR:
            fprintf(fp, ".A(%s), .B(%s), .Y(%s)",
                    em_cin(M, c->ins[0], n0, 64),
                    em_cin(M, c->ins[1], n1, 64),
                    em_cnet(M, c->out, nb, 64));
            break;

        default:
        {
            const char *opin = em_opin(lib, lc);
            uint8_t j2;
            int icnt = 0;
            for (j2 = 0; j2 < lc->n_pin && j2 < LB_MAX_PINS; j2++) {
                if (lc->pins[j2].dir == LB_DIR_IN &&
                    !lc->pins[j2].is_clk) {
                    if (icnt < (int)c->n_in) {
                        if (icnt > 0) fprintf(fp, ", ");
                        fprintf(fp, ".%.*s(%s)",
                                (int)lc->pins[j2].name_len,
                                lib->strs + lc->pins[j2].name_off,
                                em_cin(M, c->ins[icnt], n0, 64));
                    }
                    icnt++;
                }
            }
            fprintf(fp, ", .%s(%s)", opin,
                    em_cnet(M, c->out, nb, 64));
            break;
        }
        }

        fprintf(fp, " );\n");
    }

    fprintf(fp, "\nendmodule\n");

    printf("takahe: emitted %u cell instances\n", ucnt);
    return 0;
}
