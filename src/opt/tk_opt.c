/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_opt.c -- optimiser driver
 *
 * cprop then dce in a loop until the netlist stops changing or the iteration
 * limit runs out. The limit is there because a pass that oscillates would
 * otherwise never come back.
 *
 * Radix-aware: given cell definitions, cprop uses truth tables instead of
 * the hardcoded binary rules.
 */

#include "takahe.h"

int
op_opt(rt_mod_t *M, const cd_lib_t *cd)
{
    int total = 0;
    KA_GUARD(iter, 20);

    if (!M) return 0;

    while (iter--) {
        int c = op_cprop(M, cd);
        int p = op_pmatch(M);
        int d = op_dce(M);
        total += c + p + d;
        if (c == 0 && p == 0 && d == 0) break;
    }

    return total;
}
