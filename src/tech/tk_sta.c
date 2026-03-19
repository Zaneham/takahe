/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_sta.c -- Static Timing Analysis for Takahe
 *
 * Proper topological-sort STA with combinational loop detection.
 *
 * The algorithm:
 *   1. Build a dependency graph: for each cell, record which
 *      nets it reads and writes. DFFs break dependencies.
 *   2. Topological sort via Kahn's algorithm: process cells
 *      whose inputs all have computed arrival times.
 *   3. Cells left unprocessed are in combinational loops —
 *      report them as errors.
 *   4. Slack = required - arrival. Report critical path.
 *   5. Setup/hold check at every DFF.
 *
 * All arithmetic in femtoseconds. No floats. Ever.
 *
 * References (APA 7th):
 *
 * Hitchcock, R. B., Smith, G. L., & Cheng, D. D. (1982).
 *   Timing analysis of computer hardware. IBM Journal of
 *   Research and Development, 26(1), 100–105.
 *   https://doi.org/10.1147/rd.261.0100
 *
 * Sapatnekar, S. S. (2004). Timing. Springer.
 *   https://doi.org/10.1007/978-0-387-21830-1
 *
 * Bhasker, J., & Chadha, R. (2009). Static timing analysis
 *   for nanometer designs: A practical approach. Springer.
 *   https://doi.org/10.1007/978-0-387-93820-2
 *
 * Liberty timing format:
 *   Open Source Liberty (2017). Liberty Technical Reference
 *   Manual. Si2 (Silicon Integration Initiative).
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Per-net timing annotation ---- */

typedef struct {
    tk_fs_t  arr;     /* arrival time (latest signal arrival)  */
    tk_fs_t  req;     /* required time                         */
    tk_fs_t  slew;    /* output slew at this net               */
    uint32_t pred;    /* predecessor cell (for path tracing)   */
} ta_net_t;

/* ---- Cell delay lookup ---- */

static tk_fs_t
ta_cdly(const rt_cell_t *c, const lb_lib_t *lib,
        const mp_bind_t *tbl, tk_fs_t slew, tk_af_t load)
{
    rt_ctype_t ct = c->type;
    const lb_cell_t *lc;
    uint8_t j;

    /* Structural wiring — zero delay */
    if (ct == RT_ASSIGN || ct == RT_BUF || ct == RT_CONST)
        return 0;

    if (ct >= RT_CELL_COUNT || !tbl[ct].valid) return 0;
    lc = &lib->cells[tbl[ct].cell_idx];

    /* NLDM table lookup if available */
    for (j = 0; j < lc->n_pin; j++) {
        if (lc->pins[j].dir == LB_DIR_OUT) {
            if (lc->pins[j].rise.valid)
                return lb_dly(&lc->pins[j].rise, slew, load);
            break;
        }
    }

    /* Fallback: worst-case delay_max */
    return lc->delay_max;
}

/* ---- Output load (sum of fanout pin capacitances) ---- */

static tk_af_t
ta_load(const rt_mod_t *M, const lb_lib_t *lib,
        const mp_bind_t *tbl, uint32_t ni)
{
    tk_af_t total = 0;
    uint32_t ci;
    uint8_t j;

    KA_GUARD(gl, 262144);
    for (ci = 1; ci < M->n_cell && gl--; ci++) {
        const rt_cell_t *c = &M->cells[ci];
        if (c->type == RT_CELL_COUNT) continue;
        for (j = 0; j < c->n_in; j++) {
            if (c->ins[j] == ni) {
                if (c->type < RT_CELL_COUNT && tbl[c->type].valid) {
                    const lb_cell_t *lc = &lib->cells[tbl[c->type].cell_idx];
                    if (j < lc->n_pin && lc->pins[j].cap > 0)
                        total += lc->pins[j].cap;
                    else
                        total += TK_PF2AF(0.001);
                }
            }
        }
    }
    return total > 0 ? total : TK_PF2AF(0.001);
}

/* ---- Public: run STA ---- */

int
ta_sta(const rt_mod_t *M, const lb_lib_t *lib,
       const mp_bind_t *tbl, tk_fs_t clk_fs)
{
    ta_net_t *ta;
    uint32_t *indeg;   /* in-degree per net (for topo sort)   */
    uint32_t *queue;   /* Kahn's algorithm work queue         */
    uint32_t qh, qt;   /* queue head/tail                     */
    uint32_t i, ci;
    int viol = 0, loops = 0;
    tk_fs_t worst_slack;
    uint32_t worst_net = 0;
    uint32_t ncomb = 0; /* combinational (non-DFF) cells       */

    if (!M || !lib || !tbl || clk_fs <= 0) return -1;

    ta = (ta_net_t *)calloc(M->n_net, sizeof(ta_net_t));
    indeg = (uint32_t *)calloc(M->n_net, sizeof(uint32_t));
    queue = (uint32_t *)calloc(M->n_net, sizeof(uint32_t));
    if (!ta || !indeg || !queue) {
        free(ta); free(indeg); free(queue);
        return -1;
    }

    /* Initialise timing annotations */
    for (i = 1; i < M->n_net; i++) {
        ta[i].arr = 0;
        ta[i].req = clk_fs;
        ta[i].slew = TK_NS2FS(0.1);
    }

    /* Compute in-degree for each net.
     * A net's in-degree = number of input nets that must be
     * resolved before this net's arrival time can be computed.
     * DFF outputs have in-degree 0 (they start new paths).
     * Port inputs have in-degree 0 (primary inputs). */
    for (ci = 1; ci < M->n_cell; ci++) {
        const rt_cell_t *c = &M->cells[ci];
        uint8_t j;
        if (c->type == RT_CELL_COUNT) continue;
        if (c->out == 0 || c->out >= M->n_net) continue;

        /* DFFs break timing: their outputs are time-zero */
        if (c->type == RT_DFF || c->type == RT_DFFR)
            continue;

        ncomb++;
        for (j = 0; j < c->n_in; j++) {
            uint32_t ni = c->ins[j];
            if (ni > 0 && ni < M->n_net && ni != c->out)
                indeg[c->out]++;
        }
    }

    /* Seed queue with zero-in-degree nets (inputs, DFF outputs) */
    qh = qt = 0;
    for (i = 1; i < M->n_net; i++) {
        if (indeg[i] == 0)
            queue[qt++] = i;
    }

    /* Kahn's algorithm: process in topological order.
     * For each net we process, update arrival times of
     * all cells it feeds, then decrement in-degree of
     * those cells' output nets. */
    {
        uint32_t processed = 0;
        KA_GUARD(gk, 300000);
        while (qh < qt && gk--) {
            uint32_t ni = queue[qh++];
            processed++;

            /* Find all cells that read this net */
            KA_GUARD(gc, 300000);
            for (ci = 1; ci < M->n_cell && gc--; ci++) {
                const rt_cell_t *c = &M->cells[ci];
                uint8_t j;
                int feeds = 0;

                if (c->type == RT_CELL_COUNT) continue;
                if (c->type == RT_DFF || c->type == RT_DFFR) continue;
                if (c->out == 0 || c->out >= M->n_net) continue;

                /* Does this cell read net ni? */
                for (j = 0; j < c->n_in; j++) {
                    if (c->ins[j] == ni) { feeds = 1; break; }
                }
                if (!feeds) continue;

                /* Decrement in-degree of output net */
                if (indeg[c->out] > 0)
                    indeg[c->out]--;

                /* Update arrival time from this input */
                {
                    tk_fs_t in_arr = ta[ni].arr;
                    tk_fs_t delay, new_arr;
                    tk_af_t load;

                    load = ta_load(M, lib, tbl, c->out);
                    delay = ta_cdly(c, lib, tbl, ta[ni].slew, load);
                    new_arr = in_arr + delay;

                    if (new_arr > ta[c->out].arr) {
                        ta[c->out].arr = new_arr;
                        ta[c->out].pred = ci;
                        /* Propagate output slew: look up from
                         * transition table if available, else
                         * pass through input slew. */
                        ta[c->out].slew = ta[ni].slew;
                        if (c->type < RT_CELL_COUNT &&
                            tbl[c->type].valid) {
                            const lb_cell_t *tlc =
                                &lib->cells[tbl[c->type].cell_idx];
                            uint8_t k;
                            for (k = 0; k < tlc->n_pin; k++) {
                                if (tlc->pins[k].dir == LB_DIR_OUT &&
                                    tlc->pins[k].tran_r.valid) {
                                    ta[c->out].slew = lb_dly(
                                        &tlc->pins[k].tran_r,
                                        ta[ni].slew, load);
                                    break;
                                }
                            }
                        }
                    }
                }

                /* If output now has zero in-degree, enqueue it */
                if (indeg[c->out] == 0 && qt < M->n_net)
                    queue[qt++] = c->out;
            }
        }

        /* Nets still with non-zero in-degree are in loops */
        for (i = 1; i < M->n_net; i++) {
            if (indeg[i] > 0) loops++;
        }
    }

    /* Setup/hold checking at DFFs */
    {
        int setup_v = 0, hold_v = 0;
        KA_GUARD(gdff, 262144);
        for (ci = 1; ci < M->n_cell && gdff--; ci++) {
            const rt_cell_t *c = &M->cells[ci];
            tk_fs_t d_arr;
            const lb_cell_t *lc;

            if (c->type != RT_DFF && c->type != RT_DFFR) continue;
            if (c->n_in < 1) continue;

            /* D input arrival time */
            d_arr = (c->ins[0] > 0 && c->ins[0] < M->n_net) ?
                    ta[c->ins[0]].arr : 0;

            /* Get setup time from Liberty cell */
            lc = tbl[c->type].valid ?
                 &lib->cells[tbl[c->type].cell_idx] : NULL;

            if (lc && lc->setup > 0) {
                /* Setup check: data must arrive before clk - setup */
                if (d_arr > clk_fs - lc->setup) {
                    setup_v++;
                    viol++;
                }
            } else {
                /* No setup data: check against clock period */
                if (d_arr > clk_fs) {
                    setup_v++;
                    viol++;
                }
            }

            if (lc && lc->hold > 0) {
                /* Hold check: data must be stable after clk + hold */
                if (d_arr < lc->hold) {
                    hold_v++;
                    viol++;
                }
            }
        }

        if (setup_v > 0 || hold_v > 0) {
            printf("takahe: %d setup violation(s), %d hold violation(s)\n",
                   setup_v, hold_v);
        }
    }

    /* Find worst slack across all nets */
    worst_slack = clk_fs;
    for (i = 1; i < M->n_net; i++) {
        tk_fs_t slack;
        if (indeg[i] > 0) continue;  /* skip loop nets */
        slack = ta[i].req - ta[i].arr;
        if (slack < worst_slack) {
            worst_slack = slack;
            worst_net = i;
        }
        if (slack < 0) viol++;
    }

    /* Report */
    printf("\n--- Static Timing Analysis ---\n");
    printf("Clock period: %" PRId64 " fs (%.3f ns)\n",
           clk_fs, (double)clk_fs / 1e6);

    if (loops > 0)
        printf("WARNING: %d nets in combinational loops (skipped)\n",
               loops);

    if (worst_net > 0 && worst_net < M->n_net) {
        const rt_net_t *wn = &M->nets[worst_net];
        printf("Critical path: %.*s (net %u)\n",
               (int)wn->name_len, M->strs + wn->name_off, worst_net);
        printf("  Arrival:  %" PRId64 " fs (%.3f ns)\n",
               ta[worst_net].arr, (double)ta[worst_net].arr / 1e6);
        printf("  Required: %" PRId64 " fs (%.3f ns)\n",
               ta[worst_net].req, (double)ta[worst_net].req / 1e6);
        printf("  Slack:    %" PRId64 " fs (%.3f ns) %s\n",
               worst_slack, (double)worst_slack / 1e6,
               worst_slack >= 0 ? "MET" : "VIOLATED");
    }

    if (viol > 0)
        printf("  %d timing violation(s)\n", viol);
    else
        printf("  All paths meet timing.\n");

    /* Path trace */
    if (worst_net > 0) {
        uint32_t ni = worst_net;
        uint32_t prev_ni = 0;
        int depth = 0;
        printf("  Path:\n");
        KA_GUARD(gpath, 100);
        while (ni > 0 && ni < M->n_net && depth < 30 && gpath--) {
            const rt_net_t *n = &M->nets[ni];
            uint32_t drv = ta[ni].pred;

            if (ni == prev_ni) break;
            printf("    %2d. %.*s (arr=%.3f ns, dly=%.3f ns)\n",
                   depth, (int)n->name_len, M->strs + n->name_off,
                   (double)ta[ni].arr / 1e6,
                   depth > 0 ?
                       (double)(ta[prev_ni].arr - ta[ni].arr) / 1e6 : 0.0);

            depth++;
            prev_ni = ni;
            if (drv == 0 || drv >= M->n_cell) break;
            if (ta[ni].arr <= 0) break;

            {
                const rt_cell_t *dc = &M->cells[drv];
                uint32_t best = 0;
                tk_fs_t best_arr = -1;
                uint8_t j;
                for (j = 0; j < dc->n_in; j++) {
                    uint32_t inp = dc->ins[j];
                    if (inp > 0 && inp < M->n_net &&
                        inp != ni && ta[inp].arr > best_arr) {
                        best_arr = ta[inp].arr;
                        best = inp;
                    }
                }
                if (best == 0) break;
                ni = best;
            }
        }
    }

    free(queue);
    free(indeg);
    free(ta);
    return viol;
}
