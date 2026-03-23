/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_fpga.c -- FPGA LUT mapping for Takahe
 *
 * Maps the bit-blasted netlist to 4-LUTs for iCE40 FPGAs.
 * A 4-LUT is just a 16-entry ROM addressed by 4 wires —
 * it can implement literally any boolean function of 4 inputs.
 * The INIT value IS the truth table, packed into 16 bits.
 *
 * The output is nextpnr JSON, which feeds straight into
 * nextpnr-ice40 for place and route. No Yosys needed.
 * A $5 Lattice board can run your ternary ALU. The future
 * is weird and I'm here for it.
 *
 * The Sumerians had lookup tables too. Theirs were clay
 * tablets with multiplication results. Same prinicple,
 * different substrate.
 *
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Compute LUT INIT value from cell type ----
 * For a k-input gate, enumerate all 2^k input combinations
 * and evaluate the gate function. Returns the INIT bitmask.
 * Inputs map to LUT address bits: in[0]=bit0, in[1]=bit1, etc. */

static uint16_t
fp_init(rt_ctype_t type, uint8_t n_in)
{
    uint16_t init = 0;
    uint16_t i;
    uint16_t max = (uint16_t)(1u << n_in);

    for (i = 0; i < max && i < 16; i++) {
        uint8_t a = (i >> 0) & 1;
        uint8_t b = (i >> 1) & 1;
        uint8_t c = (i >> 2) & 1;
        uint8_t r = 0;

        switch ((int)type) {
        case RT_AND:   r = a & b; break;
        case RT_OR:    r = a | b; break;
        case RT_XOR:   r = a ^ b; break;
        case RT_NAND:  r = (uint8_t)(!(a & b)); break;
        case RT_NOR:   r = (uint8_t)(!(a | b)); break;
        case RT_XNOR:  r = (uint8_t)(!(a ^ b)); break;
        case RT_NOT:   r = (uint8_t)(!a); break;
        case RT_BUF:   r = a; break;
        case RT_ASSIGN:r = a; break;
        /* MUX: ins[0]=sel, ins[1]=d0, ins[2]=d1
         * In LUT: bit0=sel(a), bit1=d0(b), bit2=d1(c)
         * Output = a ? c : b */
        case RT_MUX:   r = a ? c : b; break;
        default:       r = 0; break;
        }

        if (r) init |= (uint16_t)(1u << i);
    }

    return init;
}

/* ---- Emit nextpnr JSON for iCE40 ----
 * Format spec: https://github.com/YosysHQ/nextpnr
 * Cells are SB_LUT4 (combinational) or SB_DFF (sequential).
 * Nets connect cell ports. Ports are the top-level I/O. */

int
fp_json(const rt_mod_t *M, FILE *fp)
{
    uint32_t i;
    uint32_t lut_cnt = 0, dff_cnt = 0;
    int first;

    if (!M || !fp) return -1;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"creator\": \"Takahe v0.1.0 FPGA\",\n");
    fprintf(fp, "  \"modules\": {\n");
    fprintf(fp, "    \"%s\": {\n", M->mod_name[0] ? M->mod_name : "top");

    /* ---- Ports ---- */
    fprintf(fp, "      \"ports\": {\n");
    first = 1;
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        const char *dir;
        if (n->is_port == 0) continue;
        dir = (n->is_port == 1) ? "input" : "output";
        if (!first) fprintf(fp, ",\n");
        first = 0;
        fprintf(fp, "        \"%.*s\": { \"direction\": \"%s\", \"bits\": [ %u ] }",
                (int)n->name_len, M->strs + n->name_off, dir, i);
    }
    fprintf(fp, "\n      },\n");

    /* ---- Cells ---- */
    fprintf(fp, "      \"cells\": {\n");
    first = 1;
    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        rt_ctype_t ct;

        if (c->type == RT_CELL_COUNT) continue;
        ct = c->type;

        /* Skip width > 1 (should be bit-blasted already) */
        if (c->width > 1) continue;

        if (ct == RT_CONST) {
            /* Constant: LUT with all-0 or all-1 output */
            uint16_t init = c->param ? 0xFFFF : 0x0000;
            if (!first) fprintf(fp, ",\n");
            first = 0;
            fprintf(fp, "        \"lut%u\": {\n", lut_cnt++);
            fprintf(fp, "          \"type\": \"SB_LUT4\",\n");
            fprintf(fp, "          \"parameters\": { \"LUT_INIT\": \"16'h%04X\" },\n",
                    init);
            fprintf(fp, "          \"port_directions\": { \"O\": \"output\" },\n");
            fprintf(fp, "          \"connections\": { \"O\": [ %u ] }\n",
                    c->out);
            fprintf(fp, "        }");
            continue;
        }

        if (ct == RT_DFF || ct == RT_DFFR) {
            /* DFF: SB_DFF or SB_DFFR */
            if (!first) fprintf(fp, ",\n");
            first = 0;
            fprintf(fp, "        \"dff%u\": {\n", dff_cnt++);
            fprintf(fp, "          \"type\": \"%s\",\n",
                    ct == RT_DFFR ? "SB_DFFR" : "SB_DFF");
            fprintf(fp, "          \"port_directions\": { "
                    "\"D\": \"input\", \"C\": \"input\", \"Q\": \"output\"");
            if (ct == RT_DFFR)
                fprintf(fp, ", \"R\": \"input\"");
            fprintf(fp, " },\n");
            fprintf(fp, "          \"connections\": { "
                    "\"D\": [ %u ], \"C\": [ %u ], \"Q\": [ %u ]",
                    c->ins[0], c->ins[1], c->out);
            if (ct == RT_DFFR && c->n_in >= 3)
                fprintf(fp, ", \"R\": [ %u ]", c->ins[2]);
            fprintf(fp, " }\n");
            fprintf(fp, "        }");
            continue;
        }

        /* Combinational: map to SB_LUT4 */
        {
            uint16_t init = fp_init(ct, c->n_in);
            if (!first) fprintf(fp, ",\n");
            first = 0;
            fprintf(fp, "        \"lut%u\": {\n", lut_cnt++);
            fprintf(fp, "          \"type\": \"SB_LUT4\",\n");
            fprintf(fp, "          \"parameters\": { \"LUT_INIT\": \"16'h%04X\" },\n",
                    init);
            fprintf(fp, "          \"port_directions\": { "
                    "\"I0\": \"input\", \"I1\": \"input\", "
                    "\"I2\": \"input\", \"I3\": \"input\", "
                    "\"O\": \"output\" },\n");
            fprintf(fp, "          \"connections\": { ");
            /* Map cell inputs to LUT inputs, unused tied to [0] */
            fprintf(fp, "\"I0\": [ %u ]", c->n_in >= 1 ? c->ins[0] : 0);
            fprintf(fp, ", \"I1\": [ %u ]", c->n_in >= 2 ? c->ins[1] : 0);
            fprintf(fp, ", \"I2\": [ %u ]", c->n_in >= 3 ? c->ins[2] : 0);
            fprintf(fp, ", \"I3\": [ 0 ]");  /* unused 4th input */
            fprintf(fp, ", \"O\": [ %u ]", c->out);
            fprintf(fp, " }\n");
            fprintf(fp, "        }");
        }
    }
    fprintf(fp, "\n      },\n");

    /* ---- Netnames ---- */
    fprintf(fp, "      \"netnames\": {\n");
    first = 1;
    for (i = 1; i < M->n_net; i++) {
        const rt_net_t *n = &M->nets[i];
        if (n->name_len == 0) continue;
        if (!first) fprintf(fp, ",\n");
        first = 0;
        fprintf(fp, "        \"%.*s\": { \"bits\": [ %u ] }",
                (int)n->name_len, M->strs + n->name_off, i);
    }
    fprintf(fp, "\n      }\n");

    fprintf(fp, "    }\n");
    fprintf(fp, "  }\n");
    fprintf(fp, "}\n");

    printf("takahe: FPGA: %u LUTs, %u DFFs\n", lut_cnt, dff_cnt);
    return 0;
}
