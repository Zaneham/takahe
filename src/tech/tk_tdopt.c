/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_tdopt.c -- Timing-driven optimisation for Takahe
 *
 * Walks the critical path and upsizes cells until timing
 * is met. The SKY130 library has multiple drive strengths
 * per gate type: and2_0 (tiny, slow), and2_1 (small),
 * and2_2 (medium), and2_4 (large, fast). Bigger transistors
 * drive more load, which means shorter delay, which means
 * the signal arrives before the clock edge, which means
 * the chip works at the frequency you told your boss it
 * would work at.
 *
 * The algorithm:
 *   1. Run STA, find critical path
 *   2. For each cell on the critical path, find a larger
 *      variant with the same logic function
 *   3. Swap the binding, re-run STA
 *   4. Repeat until timing met or no improvement
 *
 * This is "gate sizing" in the literature. Synopsys charges
 * a significant fraction of a house for it. We do it in
 * 150 lines.
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Find a faster variant of a library cell ----
 * Given a cell index in the library, find another cell
 * with the same base name but a higher drive strength.
 * SKY130 naming: sky130_fd_sc_hd__and2_1 → and2_2.
 * The suffix number is the drive strength. */

static uint32_t
td_fdup(const lb_lib_t *lib, uint32_t ci)
{
    const lb_cell_t *orig = &lib->cells[ci];
    const char *oname = lib->strs + orig->name_off;
    uint16_t olen = orig->name_len;
    uint32_t best = 0;
    float best_area = 1e30f;
    uint32_t i;
    int base_len;

    /* Find the last '_' followed by a digit — that's the
     * drive strength suffix. Everything before it is the
     * base name that must match. */
    base_len = (int)olen;
    {
        int j;
        for (j = (int)olen - 1; j > 0; j--) {
            if (oname[j] == '_') {
                base_len = j + 1;
                break;
            }
        }
    }

    /* Scan library for cells with same base, larger area
     * (larger area = larger transistors = faster) */
    KA_GUARD(gs, 512);
    for (i = 0; i < lib->n_cell && gs--; i++) {
        const lb_cell_t *cand = &lib->cells[i];
        const char *cname;

        if (i == ci) continue;
        if (cand->name_len < (uint16_t)base_len) continue;
        if (cand->kind != orig->kind) continue;
        if (cand->n_in != orig->n_in) continue;

        cname = lib->strs + cand->name_off;
        if (memcmp(cname, oname, (size_t)base_len) != 0)
            continue;

        /* Same base name, different drive strength.
         * Pick the smallest one that's still larger than orig. */
        if (cand->area > orig->area && cand->area < best_area) {
            best = i;
            best_area = cand->area;
        }
    }

    return best; /* 0 = no larger variant found */
}

/* ---- Public: timing-driven gate sizing ---- */

int
op_tdopt(rt_mod_t *M, const lb_lib_t *lib,
         mp_bind_t *tbl, tk_fs_t clk_fs)
{
    int iter, total = 0;

    if (!M || !lib || !tbl || clk_fs <= 0) return 0;

    KA_GUARD(giter, 20);
    for (iter = 0; giter--; iter++) {
        /* Run STA to find critical path */
        tk_fs_t *arr;
        uint32_t *pred;
        uint32_t worst_net = 0;
        tk_fs_t worst_slack = clk_fs;
        uint32_t i, ci;
        int resized = 0;

        /* Simple STA: forward pass only, find worst slack */
        arr = (tk_fs_t *)calloc(M->n_net, sizeof(tk_fs_t));
        pred = (uint32_t *)calloc(M->n_net, sizeof(uint32_t));
        if (!arr || !pred) { free(arr); free(pred); break; }

        /* Forward pass (simplified — no topo sort, just
         * iterate until stable. Bounded.) */
        {
            int changed;
            KA_GUARD(gfwd, 50);
            do {
                changed = 0;
                KA_GUARD(gc, 262144);
                for (ci = 1; ci < M->n_cell && gc--; ci++) {
                    const rt_cell_t *c = &M->cells[ci];
                    tk_fs_t max_in = 0, delay, new_arr;
                    uint8_t j;

                    if (c->type == RT_CELL_COUNT) continue;
                    if (c->out == 0 || c->out >= M->n_net) continue;
                    if (c->type == RT_DFF || c->type == RT_DFFR) continue;
                    if (c->type == RT_ASSIGN || c->type == RT_BUF ||
                        c->type == RT_CONST) continue;

                    for (j = 0; j < c->n_in && j < RT_MAX_PIN; j++) {
                        uint32_t ni = c->ins[j];
                        if (ni > 0 && ni < M->n_net &&
                            arr[ni] > max_in)
                            max_in = arr[ni];
                    }

                    /* Get delay from bound cell */
                    delay = 0;
                    if (c->type < RT_CELL_COUNT && tbl[c->type].valid) {
                        const lb_cell_t *lc =
                            &lib->cells[tbl[c->type].cell_idx];
                        delay = lc->delay_max;
                    }

                    new_arr = max_in + delay;
                    if (new_arr > clk_fs * 100)
                        new_arr = clk_fs * 100;

                    if (new_arr > arr[c->out]) {
                        arr[c->out] = new_arr;
                        pred[c->out] = ci;
                        changed = 1;
                    }
                }
            } while (changed && gfwd--);
        }

        /* Find worst slack */
        for (i = 1; i < M->n_net; i++) {
            tk_fs_t slack = clk_fs - arr[i];
            if (slack < worst_slack) {
                worst_slack = slack;
                worst_net = i;
            }
        }

        /* If timing is met, we're done. Victory lap. */
        if (worst_slack >= 0) {
            free(arr); free(pred);
            if (total > 0)
                printf("takahe: timing met after %d resizing passes\n",
                       iter);
            break;
        }

        /* Walk critical path, try to upsize cells */
        {
            uint32_t ni = worst_net;
            KA_GUARD(gwalk, 100);
            while (ni > 0 && ni < M->n_net && gwalk--) {
                uint32_t drv = pred[ni];
                if (drv == 0 || drv >= M->n_cell) break;

                {
                    rt_cell_t *c = &M->cells[drv];
                    rt_ctype_t ct = c->type;

                    if (ct < RT_CELL_COUNT && tbl[ct].valid) {
                        uint32_t faster = td_fdup(lib,
                            tbl[ct].cell_idx);
                        if (faster > 0) {
                            /* Journal the binding change so we
                             * can roll back if timing gets worse.
                             * CICS: log before modify. */
                            jr_mbind(tbl, (uint8_t)ct);
                            tbl[ct].cell_idx = faster;
                            resized++;
                            total++;
                        }
                    }
                }

                /* Walk to predecessor's worst input */
                {
                    const rt_cell_t *dc = &M->cells[drv];
                    uint32_t best = 0;
                    tk_fs_t best_arr = -1;
                    uint8_t j;
                    for (j = 0; j < dc->n_in && j < RT_MAX_PIN; j++) {
                        uint32_t inp = dc->ins[j];
                        if (inp > 0 && inp < M->n_net &&
                            inp != ni && arr[inp] > best_arr) {
                            best_arr = arr[inp];
                            best = inp;
                        }
                    }
                    if (best == 0) break;
                    ni = best;
                }
            }
        }

        free(arr); free(pred);

        if (resized == 0) break; /* can't improve further */
    }

    if (total > 0)
        printf("takahe: timing-driven: %d cells upsized\n", total);
    return total;
}
