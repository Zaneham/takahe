/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_opt.c -- Optimiser driver for Takahe RTL
 *
 * Calls cprop then dce in a loop until the netlist stops
 * changing or we hit the iteration limit. Like shaking a
 * tree until no more dead branches fall off — but with a
 * hard limit so we don't stand here all day.
 *
 * Now radix-aware: if cell definitions are provided, cprop
 * uses truth tables instead of hardcoded binary rules.
 * Ternary AND(-1, x) = -1 falls out automatically.
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
