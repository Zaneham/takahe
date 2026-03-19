/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_yosys.c -- Yosys JSON netlist emitter for Takahe RTL
 *
 * The lingua franca of open-source EDA. If BLIF is Esperanto,
 * Yosys JSON is English: everybody actually uses it, even if
 * they pretend otherwise. Multi-bit signals are first-class,
 * which means we don't have to pretend everything is 1-bit
 * like it's 1985 and we're running on a VAX.
 *
 * Output is consumable by nextpnr, OpenROAD, and Yosys itself.
 * Format reference: Yosys manual, Appendix C.
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Yosys cell type name from RTL cell type ---- */

static const char *
ys_ctyp(rt_ctype_t t)
{
    switch (t) {
    case RT_AND:    return "$and";
    case RT_OR:     return "$or";
    case RT_XOR:    return "$xor";
    case RT_NAND:   return "$nand";
    case RT_NOR:    return "$nor";
    case RT_XNOR:   return "$xnor";
    case RT_NOT:    return "$not";
    case RT_BUF:    return "$buf";
    case RT_MUX:    return "$mux";
    case RT_ADD:    return "$add";
    case RT_SUB:    return "$sub";
    case RT_MUL:    return "$mul";
    case RT_SHL:    return "$shl";
    case RT_SHR:    return "$shr";
    case RT_SHRA:   return "$sshr";
    case RT_EQ:     return "$eq";
    case RT_NE:     return "$ne";
    case RT_LT:     return "$lt";
    case RT_LE:     return "$le";
    case RT_GT:     return "$gt";
    case RT_GE:     return "$ge";
    case RT_DFF:    return "$dff";
    case RT_DFFR:   return "$adff";
    case RT_DLAT:   return "$dlatch";
    case RT_CONCAT: return "$concat";
    case RT_SELECT: return "$slice";
    case RT_PMUX:   return "$pmux";
    case RT_MEMRD:  return "$memrd";
    case RT_MEMWR:  return "$memwr";
    case RT_ASSIGN: return "$buf";
    case RT_CONST:  return NULL;  /* inline, no cell */
    case RT_CELL_COUNT: return NULL;
    default:        return NULL;
    }
}

/* ---- Emit a JSON bits array for a net ----
 * Each net of width w gets IDs [base, base+w).
 * Constant 0 = "0", constant 1 = "1". */

static void
ys_bits(FILE *fp, const rt_mod_t *M, uint32_t ni,
        const uint32_t *bmap)
{
    uint32_t w, base, j;

    if (ni == 0 || ni >= M->n_net) {
        fprintf(fp, "[0]");
        return;
    }
    w = M->nets[ni].width;
    if (w == 0) w = 1;

    /* Check if driven by CONST */
    {
        uint32_t drv = M->nets[ni].driver;
        if (drv > 0 && drv < M->n_cell &&
            M->cells[drv].type == RT_CONST) {
            int64_t val = M->cells[drv].param;
            fprintf(fp, "[");
            for (j = 0; j < w; j++) {
                if (j > 0) fprintf(fp, ", ");
                fprintf(fp, "\"%c\"", (val >> j) & 1 ? '1' : '0');
            }
            fprintf(fp, "]");
            return;
        }
    }

    base = bmap[ni];
    fprintf(fp, "[");
    for (j = 0; j < w; j++) {
        if (j > 0) fprintf(fp, ", ");
        fprintf(fp, "%u", base + j);
    }
    fprintf(fp, "]");
}

/* ---- Emit a JSON string, escaped ---- */

static void
ys_str(FILE *fp, const char *s, uint16_t len)
{
    uint16_t i;
    fputc('"', fp);
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"') fprintf(fp, "\\\"");
        else if (c == '\\') fprintf(fp, "\\\\");
        else if (c == '\n') fprintf(fp, "\\n");
        else fputc(c, fp);
    }
    fputc('"', fp);
}

/* ---- Public: emit Yosys JSON to file ---- */

int
mp_yosys(const rt_mod_t *M, FILE *fp)
{
    uint32_t i;
    uint32_t *bmap;   /* net index → base bit ID */
    uint32_t next_id;
    int first;

    if (!M || !fp) return -1;

    /* Assign bit IDs: 0 and 1 reserved for constants.
     * Each net i of width w gets IDs [next_id, next_id+w). */
    bmap = (uint32_t *)calloc(M->n_net, sizeof(uint32_t));
    if (!bmap) return -1;

    next_id = 2;
    for (i = 1; i < M->n_net; i++) {
        uint32_t w = M->nets[i].width;
        if (w == 0) w = 1;
        /* Skip CONST-driven nets — they're inlined */
        {
            uint32_t drv = M->nets[i].driver;
            if (drv > 0 && drv < M->n_cell &&
                M->cells[drv].type == RT_CONST) {
                bmap[i] = 0;
                continue;
            }
        }
        bmap[i] = next_id;
        next_id += w;
    }

    /* Top-level object */
    fprintf(fp, "{\n");
    fprintf(fp, "  \"creator\": \"Takahe v%d.%d.%d\",\n",
            TK_VER_MAJOR, TK_VER_MINOR, TK_VER_PATCH);
    fprintf(fp, "  \"modules\": {\n");
    fprintf(fp, "    \"takahe_top\": {\n");

    /* ---- Ports ---- */
    fprintf(fp, "      \"ports\": {\n");
    first = 1;
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        const char *dir;
        if (n->is_port == 0) continue;

        if (n->is_port == 1) dir = "input";
        else if (n->is_port == 2) dir = "output";
        else dir = "inout";

        if (!first) fprintf(fp, ",\n");
        first = 0;
        fprintf(fp, "        ");
        ys_str(fp, M->strs + n->name_off, n->name_len);
        fprintf(fp, ": { \"direction\": \"%s\", \"bits\": ", dir);
        ys_bits(fp, M, i, bmap);
        fprintf(fp, " }");
    }
    fprintf(fp, "\n      },\n");

    /* ---- Cells ---- */
    fprintf(fp, "      \"cells\": {\n");
    first = 1;
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        const char *ctn;

        if (c->type == RT_CELL_COUNT) continue;
        if (c->type == RT_CONST) continue;  /* inlined */

        ctn = ys_ctyp(c->type);
        if (!ctn) continue;

        if (!first) fprintf(fp, ",\n");
        first = 0;

        fprintf(fp, "        \"$c%u\": {\n", i);
        fprintf(fp, "          \"type\": \"%s\",\n", ctn);

        /* Port directions */
        fprintf(fp, "          \"port_directions\": {");
        if (c->type == RT_MUX) {
            fprintf(fp, " \"S\": \"input\", \"A\": \"input\","
                        " \"B\": \"input\", \"Y\": \"output\"");
        } else if (c->type == RT_DFF || c->type == RT_DFFR) {
            fprintf(fp, " \"D\": \"input\", \"CLK\": \"input\","
                        " \"Q\": \"output\"");
            if (c->type == RT_DFFR)
                fprintf(fp, ", \"ARST\": \"input\"");
        } else if (c->type == RT_MEMRD) {
            fprintf(fp, " \"ADDR\": \"input\", \"DATA\": \"output\"");
        } else if (c->type == RT_MEMWR) {
            fprintf(fp, " \"ADDR\": \"input\", \"DATA\": \"input\","
                        " \"EN\": \"input\"");
        } else {
            /* Generic: inputs A, B; output Y */
            if (c->n_in >= 1) fprintf(fp, " \"A\": \"input\"");
            if (c->n_in >= 2) fprintf(fp, ", \"B\": \"input\"");
            fprintf(fp, ", \"Y\": \"output\"");
        }
        fprintf(fp, " },\n");

        /* Parameters */
        fprintf(fp, "          \"parameters\": {");
        fprintf(fp, " \"WIDTH\": %u", c->width);
        if (c->type == RT_SELECT)
            fprintf(fp, ", \"OFFSET\": %" PRId64, c->param & 0xFFFF);
        if (c->type == RT_MEMRD || c->type == RT_MEMWR)
            fprintf(fp, ", \"MEMID\": %" PRId64, c->param);
        fprintf(fp, " },\n");

        /* Connections */
        fprintf(fp, "          \"connections\": {");
        if (c->type == RT_MUX && c->n_in == 3) {
            fprintf(fp, " \"S\": ");
            ys_bits(fp, M, c->ins[0], bmap);
            fprintf(fp, ", \"A\": ");
            ys_bits(fp, M, c->ins[1], bmap);
            fprintf(fp, ", \"B\": ");
            ys_bits(fp, M, c->ins[2], bmap);
            fprintf(fp, ", \"Y\": ");
            ys_bits(fp, M, c->out, bmap);
        } else if (c->type == RT_DFF && c->n_in >= 2) {
            fprintf(fp, " \"D\": ");
            ys_bits(fp, M, c->ins[0], bmap);
            fprintf(fp, ", \"CLK\": ");
            ys_bits(fp, M, c->ins[1], bmap);
            fprintf(fp, ", \"Q\": ");
            ys_bits(fp, M, c->out, bmap);
        } else if (c->type == RT_DFFR && c->n_in >= 3) {
            fprintf(fp, " \"D\": ");
            ys_bits(fp, M, c->ins[0], bmap);
            fprintf(fp, ", \"CLK\": ");
            ys_bits(fp, M, c->ins[1], bmap);
            fprintf(fp, ", \"ARST\": ");
            ys_bits(fp, M, c->ins[2], bmap);
            fprintf(fp, ", \"Q\": ");
            ys_bits(fp, M, c->out, bmap);
        } else if (c->type == RT_MEMRD && c->n_in >= 1) {
            fprintf(fp, " \"ADDR\": ");
            ys_bits(fp, M, c->ins[0], bmap);
            fprintf(fp, ", \"DATA\": ");
            ys_bits(fp, M, c->out, bmap);
        } else if (c->type == RT_MEMWR && c->n_in >= 2) {
            fprintf(fp, " \"ADDR\": ");
            ys_bits(fp, M, c->ins[0], bmap);
            fprintf(fp, ", \"DATA\": ");
            ys_bits(fp, M, c->ins[1], bmap);
        } else {
            /* Generic: A, [B], Y */
            if (c->n_in >= 1) {
                fprintf(fp, " \"A\": ");
                ys_bits(fp, M, c->ins[0], bmap);
            }
            if (c->n_in >= 2) {
                fprintf(fp, ", \"B\": ");
                ys_bits(fp, M, c->ins[1], bmap);
            }
            fprintf(fp, ", \"Y\": ");
            ys_bits(fp, M, c->out, bmap);
        }
        fprintf(fp, " }\n");
        fprintf(fp, "        }");
    }
    fprintf(fp, "\n      },\n");

    /* ---- Netnames ---- */
    fprintf(fp, "      \"netnames\": {\n");
    first = 1;
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        /* Skip anonymous tmp/const/sel/cat nets */
        if (n->name_len <= 4) {
            const char *nm = M->strs + n->name_off;
            if (memcmp(nm, "tmp", 3) == 0 ||
                memcmp(nm, "const", 5) == 0 ||
                memcmp(nm, "sel", 3) == 0 ||
                memcmp(nm, "cat", 3) == 0 ||
                memcmp(nm, "mrd", 3) == 0 ||
                memcmp(nm, "mwr", 3) == 0 ||
                memcmp(nm, "dff_d", 5) == 0)
                continue;
        }
        /* Skip CONST-driven nets */
        {
            uint32_t drv = n->driver;
            if (drv > 0 && drv < M->n_cell &&
                M->cells[drv].type == RT_CONST)
                continue;
        }

        if (!first) fprintf(fp, ",\n");
        first = 0;
        fprintf(fp, "        ");
        ys_str(fp, M->strs + n->name_off, n->name_len);
        fprintf(fp, ": { \"bits\": ");
        ys_bits(fp, M, i, bmap);
        fprintf(fp, " }");
    }
    fprintf(fp, "\n      }\n");

    /* Close module and top */
    fprintf(fp, "    }\n");
    fprintf(fp, "  }\n");
    fprintf(fp, "}\n");

    free(bmap);
    return 0;
}
