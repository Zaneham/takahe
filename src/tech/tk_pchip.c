/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_pchip.c -- PCHIP interpolation in exact integer arithmetic
 *
 * Ported from SLATEC's PCHIM + CHFEV to C99 integer arithmetic.
 *
 * Uses int64 femtoseconds throughout. Intermediate products use
 * split multiply-then-divide to avoid overflow without __int128.
 * Derivatives stored as slopes × 10^6 (PPM) for fractional
 * precision within int64 range.
 *
 * References (APA 7th):
 *
 * Fritsch, F. N., & Carlson, R. E. (1980). Monotone piecewise
 *   cubic interpolation. SIAM Journal on Numerical Analysis,
 *   17(2), 238–246. https://doi.org/10.1137/0717021
 *
 * Fritsch, F. N., & Butland, J. (1984). A method for
 *   constructing local monotone piecewise cubic interpolants.
 *   SIAM Journal on Scientific and Statistical Computing, 5(2),
 *   300–304. https://doi.org/10.1137/0905021
 *
 * Brodlie, K. W. (1980). A review of methods for curve and
 *   function drawing. In K. W. Brodlie (Ed.), Mathematical
 *   methods in computer graphics and design (pp. 1–37).
 *   Academic Press.
 *
 * Original SLATEC implementation:
 *   Fritsch, F. N. (1982). PCHIP: Piecewise Cubic Hermite
 *   Interpolation Package [Computer software]. Lawrence
 *   Livermore National Laboratory. SLATEC Common Mathematical
 *   Library, Version 4.1.
 *
 * Modernised SLATEC source:
 *   Hambly, Z. (2026). SLATEC-Modern [Computer software].
 *   https://github.com/Zaneham/slatec-modern
 */

#include "takahe.h"

/* ---- Scale factor for derivatives ----
 * Derivatives have units of fs/fs (or aF/aF) — dimensionless
 * slopes. We scale by 10^6 to keep integer precision. */

#define PC_PPM  1000000LL

/* ---- Sign test: Fritsch's overflow-safe trick ---- */

static int
pc_sgn(int64_t a, int64_t b)
{
    if (a == 0 || b == 0) return 0;
    return ((a > 0) == (b > 0)) ? 1 : -1;
}

/* ---- Scaled division: (a * PPM) / b ----
 * Splits to avoid overflow without __int128.
 * Exact for |a| < 9.2 × 10^12 (any practical fs value). */

static int64_t
pc_sdiv(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    /* Split: a = q*b + r, then (a*PPM)/b = q*PPM + (r*PPM)/b */
    {
        int64_t q = a / b;
        int64_t r = a % b;
        return q * PC_PPM + (r * PC_PPM) / b;
    }
}

/* ---- Wide multiply: (a * b) / PPM ----
 * For two PPM-scaled values; result is PPM-scaled. */

static int64_t
pc_wmul(int64_t a, int64_t b)
{
    /* Split a = hi*PPM + lo, then a*b/PPM = hi*b + lo*b/PPM */
    int64_t hi = a / PC_PPM;
    int64_t lo = a % PC_PPM;
    return hi * b + (lo * b) / PC_PPM;
}

/* ---- Absolute value ---- */

static int64_t
pc_abs(int64_t v)
{
    return v < 0 ? -v : v;
}

/* ---- PCHIM: compute monotone derivatives ----
 * Derivatives stored as scaled slopes (× PPM). */

int
pc_deriv(const int64_t *x, const int64_t *f, int64_t *d, int n)
{
    int i, sw = 0;
    int64_t h1, h2, del1, del2, hsum;
    int64_t dmax, dmin, w1, w2, hst3;
    int64_t drat1, drat2, dsave;

    if (n < 2) return -1;

    if (n == 2) {
        d[0] = d[1] = pc_sdiv(f[1] - f[0], x[1] - x[0]);
        return 0;
    }

    h1 = x[1] - x[0];
    del1 = pc_sdiv(f[1] - f[0], h1);
    dsave = del1;

    h2 = x[2] - x[1];
    del2 = pc_sdiv(f[2] - f[1], h2);

    /* Boundary: non-centered three-point, shape-preserving */
    hsum = h1 + h2;
    w1 = pc_sdiv(h1 + hsum, hsum);
    w2 = pc_sdiv(-h1, hsum);
    d[0] = pc_wmul(w1, del1) + pc_wmul(w2, del2);

    if (pc_sgn(d[0], del1) <= 0)
        d[0] = 0;
    else if (pc_sgn(del1, del2) < 0) {
        dmax = 3 * del1;
        if (pc_abs(d[0]) > pc_abs(dmax)) d[0] = dmax;
    }

    /* Interior: Brodlie modification of Butland formula */
    for (i = 1; i < n - 1; i++) {
        if (i >= 2) {
            h1 = h2;
            h2 = x[i + 1] - x[i];
            hsum = h1 + h2;
            del1 = del2;
            del2 = pc_sdiv(f[i + 1] - f[i], h2);
        }

        d[i] = 0;

        if (pc_sgn(del1, del2) < 0) {
            sw++; dsave = del2;
        } else if (pc_sgn(del1, del2) == 0) {
            if (del2 != 0) {
                if (pc_sgn(dsave, del2) < 0) sw++;
                dsave = del2;
            }
        } else {
            /* Brodlie: weighted harmonic mean */
            hst3 = 3 * hsum;
            w1 = pc_sdiv(hsum + h1, hst3);
            w2 = pc_sdiv(hsum + h2, hst3);

            dmax = pc_abs(del1) > pc_abs(del2) ?
                   pc_abs(del1) : pc_abs(del2);
            dmin = pc_abs(del1) < pc_abs(del2) ?
                   pc_abs(del1) : pc_abs(del2);

            if (dmax == 0) continue;
            drat1 = pc_sdiv(del1, dmax);
            drat2 = pc_sdiv(del2, dmax);

            {
                int64_t wd = pc_wmul(w1, drat1) + pc_wmul(w2, drat2);
                if (wd != 0)
                    d[i] = pc_wmul(dmin, pc_sdiv(PC_PPM, wd));
                /* Restore sign from del1 (both have same sign here) */
                if (del1 < 0 && d[i] > 0) d[i] = -d[i];
                if (del1 > 0 && d[i] < 0) d[i] = -d[i];
            }
        }
    }

    /* Boundary: d[n-1] */
    w1 = pc_sdiv(-h2, hsum);
    w2 = pc_sdiv(h2 + hsum, hsum);
    d[n - 1] = pc_wmul(w1, del1) + pc_wmul(w2, del2);

    if (pc_sgn(d[n - 1], del2) <= 0)
        d[n - 1] = 0;
    else if (pc_sgn(del1, del2) < 0) {
        dmax = 3 * del2;
        if (pc_abs(d[n - 1]) > pc_abs(dmax)) d[n - 1] = dmax;
    }

    return sw;
}

/* ---- CHFEV: evaluate cubic Hermite in one interval ----
 * Fritsch's formula: f(x) = f1 + dx*(d1 + dx*(c2 + dx*c3))
 * where dx = xe - x1, and c2/c3 are cubic coefficients.
 * d1, d2 are scaled by PPM. */

int64_t
pc_eval(int64_t x1, int64_t x2, int64_t f1, int64_t f2,
        int64_t d1, int64_t d2, int64_t xe)
{
    int64_t h, dx;

    h = x2 - x1;
    if (h == 0) return f1;
    dx = xe - x1;

    /* Direct Hermite basis evaluation using __int128.
     * t = dx / h (fractional position in [0,1])
     * H00(t) = 2t³ - 3t² + 1       (value at x1)
     * H10(t) = t³ - 2t² + t         (slope at x1, × h)
     * H01(t) = -2t³ + 3t²           (value at x2)
     * H11(t) = t³ - t²              (slope at x2, × h)
     *
     * f(x) = f1*H00 + (d1*h/PPM)*H10 + f2*H01 + (d2*h/PPM)*H11
     *
     * To avoid floats: compute with numerator/denominator. */
    /* Evaluate via Fritsch's nested form (Horner's method).
     * f(x) = f1 + dx * (d1/PPM + dx * (c2/h + dx * c3/h²))
     * where c2 = (3*delta - 2*d1 - d2), c3 = (d1 + d2 - 2*delta)
     * and delta = (f2-f1)/h * PPM (slope, scaled).
     *
     * All intermediate values stay within int64 range for
     * typical NLDM values (fs < 10^12, h > 10^3). */
    {
        int64_t delta = pc_sdiv(f2 - f1, h);
        int64_t c2 = 3 * delta - 2 * d1 - d2;   /* scaled by PPM */
        int64_t c3 = d1 + d2 - 2 * delta;        /* scaled by PPM */

        /* Horner: innermost first, carefully un-scaling.
         * t3 = c2 + dx * c3 / h   (PPM-scaled)
         * t2 = d1 + dx * t3 / h   (PPM-scaled)
         * result = f1 + dx * t2 / PPM  (un-scaled to fs) */
        /* t = dx/h as a PPM-scaled fraction */
        int64_t t = pc_sdiv(dx, h);
        /* c2, c3 are PPM-scaled slopes. Horner nesting: */
        int64_t v3 = c2 + pc_wmul(t, c3);       /* PPM-scaled */
        int64_t v2 = d1 + pc_wmul(t, v3);        /* PPM-scaled */
        /* Final: f1 + h * t * v2 / PPM
         * = f1 + dx * v2 / PPM */
        return f1 + pc_wmul(dx, v2);
    }
}

/* ---- 1D lookup ---- */

int64_t
pc_lkup(const int64_t *x, const int64_t *f, int n, int64_t xe)
{
    int64_t d[32];
    int i;

    if (n < 2 || n > 32) return 0;
    if (xe <= x[0]) return f[0];
    if (xe >= x[n - 1]) return f[n - 1];

    pc_deriv(x, f, d, n);

    for (i = 1; i < n; i++)
        if (xe <= x[i]) break;
    if (i >= n) i = n - 1;

    return pc_eval(x[i-1], x[i], f[i-1], f[i], d[i-1], d[i], xe);
}

/* ---- 2D lookup: NLDM table (tensor product PCHIP) ---- */

int64_t
pc_lk2d(const int64_t *x1, int n1,
        const int64_t *x2, int n2,
        const int64_t *f,
        int64_t xe1, int64_t xe2)
{
    int64_t col[32];
    int i;

    if (n1 < 2 || n2 < 2 || n1 > 32 || n2 > 32) return 0;

    for (i = 0; i < n1; i++)
        col[i] = pc_lkup(x2, f + i * n2, n2, xe2);

    return pc_lkup(x1, col, n1, xe1);
}
