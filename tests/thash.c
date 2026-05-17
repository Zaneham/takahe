/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* thash.c -- Synthesis fingerprint tests
 *
 * The hash exists to catch the day the pipeline turns
 * non-deterministic. These tests exist to catch the day
 * the hash itself does. If something here ever fails,
 * either the canonical form changed (in which case bump
 * HS_VER and update the expected digests) or something
 * has crept into one of the upstream passes that depends
 * on pool allocation order or some other gremlin.
 */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* ---- Helper: full pipeline from string (mirrors topt.c) ---- */

static rt_mod_t *
rtl_str(const char *src)
{
    tk_lex_t *L;
    tk_parse_t *P;
    ce_val_t *cv;
    wi_val_t *wv;
    rt_mod_t *M;
    char *pp;
    uint32_t pplen, slen;

    L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    if (!L || !P) { free(L); free(P); return NULL; }
    if (tk_ldinit(L, "defs/sv_tok.def") != 0) {
        free(L); free(P); return NULL;
    }

    slen = (uint32_t)strlen(src);
    pp = (char *)malloc(slen * 2 + 256);
    if (!pp) { tk_ldfree(L); free(L); free(P); return NULL; }
    tk_preproc(src, slen, pp, slen * 2 + 256, &pplen, NULL, 0);
    tk_lex(L, pp, pplen);
    free(pp);

    tk_pinit(P, L);
    tk_parse(P);

    cv = (ce_val_t *)calloc(P->n_node, sizeof(ce_val_t));
    wv = (wi_val_t *)calloc(P->n_node, sizeof(wi_val_t));
    if (!cv || !wv) {
        free(cv); free(wv);
        tk_pfree(P); free(P);
        tk_ldfree(L); free(L);
        return NULL;
    }

    ce_eval(P, cv, P->n_node);
    el_elab(P, cv, P->n_node);
    ge_expand(P);
    fl_flat(P);
    wi_eval(P, cv, P->n_node, wv, P->n_node);

    M = lw_build(P, cv, wv, P->n_node);

    free(wv);
    free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    return M;
}

/* ---- Determinism: same source twice, same digest ----
 * The whole reason the flag exists. If this ever fails,
 * everything else that builds on canonical form (regression
 * tests, --diff, --lec verification, supply chain checks)
 * silently rots. */

static void
hs_det(void)
{
    const char *src =
        "module t(input logic a, output logic b);\n"
        "  assign b = a;\n"
        "endmodule\n";
    rt_mod_t *M1 = rtl_str(src);
    rt_mod_t *M2 = rtl_str(src);
    uint64_t h1, h2;

    CHECK(M1 != NULL);
    CHECK(M2 != NULL);

    h1 = mp_hash(M1);
    h2 = mp_hash(M2);
    CHEQ(h1, h2);
    CHNE(h1, (uint64_t)0);

    rt_free(M1); free(M1);
    rt_free(M2); free(M2);
    PASS();
}
TH_REG("hash", hs_det)

/* ---- Sensitivity: different sources, different digests ----
 * If a renamed wire collides with the original we have a
 * problem. If a wider input collides we have a bigger
 * problem. */

static void
hs_diff(void)
{
    rt_mod_t *M1 = rtl_str(
        "module t(input logic a, output logic b);\n"
        "  assign b = a;\n"
        "endmodule\n");
    rt_mod_t *M2 = rtl_str(
        "module t(input logic a, input logic c, output logic b);\n"
        "  assign b = a & c;\n"
        "endmodule\n");
    uint64_t h1, h2;

    CHECK(M1 != NULL);
    CHECK(M2 != NULL);

    h1 = mp_hash(M1);
    h2 = mp_hash(M2);
    CHNE(h1, h2);

    rt_free(M1); free(M1);
    rt_free(M2); free(M2);
    PASS();
}
TH_REG("hash", hs_diff)

/* ---- Width sensitivity: same structure, different widths,
 * different digests. A bit width change is a real netlist
 * change, the hash must agree. */

static void
hs_wide(void)
{
    rt_mod_t *M1 = rtl_str(
        "module t(input logic [7:0] a, output logic [7:0] b);\n"
        "  assign b = a;\n"
        "endmodule\n");
    rt_mod_t *M2 = rtl_str(
        "module t(input logic [15:0] a, output logic [15:0] b);\n"
        "  assign b = a;\n"
        "endmodule\n");
    uint64_t h1, h2;

    CHECK(M1 != NULL);
    CHECK(M2 != NULL);

    h1 = mp_hash(M1);
    h2 = mp_hash(M2);
    CHNE(h1, h2);

    rt_free(M1); free(M1);
    rt_free(M2); free(M2);
    PASS();
}
TH_REG("hash", hs_wide)

/* ---- Null handling: mp_hash(NULL) returns 0.
 * Not because zero is special but because the function
 * should not segfault when called defensively. */

static void
hs_null(void)
{
    CHEQ(mp_hash(NULL), (uint64_t)0);
    PASS();
}
TH_REG("hash", hs_null)

/* ---- CLI integration: --hash prints "hash: <16 hex>" and
 * two runs of the same command produce identical output.
 * This is the property a user actually relies on when they
 * pipe it to a build-system reproducibility check. */

static void
hs_cli(void)
{
    char obuf1[TH_BUFSZ];
    char obuf2[TH_BUFSZ];
    const char *p1;
    const char *p2;
    int rc1, rc2;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");

    rc1 = th_run(TK_BIN " --hash tests/smoke.sv", obuf1, TH_BUFSZ);
    rc2 = th_run(TK_BIN " --hash tests/smoke.sv", obuf2, TH_BUFSZ);
    CHEQ(rc1, 0);
    CHEQ(rc2, 0);

    p1 = strstr(obuf1, "hash: ");
    p2 = strstr(obuf2, "hash: ");
    CHECK(p1 != NULL);
    CHECK(p2 != NULL);

    /* "hash: " is 6 chars plus 16 hex digits = 22 chars to
     * compare. If both runs match on those 22 bytes the
     * digest is identical. */
    CHECK(strncmp(p1, p2, 22) == 0);

    PASS();
}
TH_REG("hash", hs_cli)

/* ---- CLI: post-opt hash stable across runs.
 * Optimisation passes have their own iteration order and
 * the hash must remain stable even when the optimiser is
 * doing real work. */

static void
hs_opt(void)
{
    char obuf1[TH_BUFSZ];
    char obuf2[TH_BUFSZ];
    const char *p1;
    const char *p2;
    int rc1, rc2;

    if (!th_exist("designs/voyager_fds.sv"))
        SKIP("no voyager_fds.sv");

    rc1 = th_run(TK_BIN " --opt --hash designs/voyager_fds.sv",
                 obuf1, TH_BUFSZ);
    rc2 = th_run(TK_BIN " --opt --hash designs/voyager_fds.sv",
                 obuf2, TH_BUFSZ);
    CHEQ(rc1, 0);
    CHEQ(rc2, 0);

    p1 = strstr(obuf1, "hash: ");
    p2 = strstr(obuf2, "hash: ");
    CHECK(p1 != NULL);
    CHECK(p2 != NULL);
    CHECK(strncmp(p1, p2, 22) == 0);

    PASS();
}
TH_REG("hash", hs_opt)
