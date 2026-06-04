/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tmmap.c -- Memory primitive library and matcher tests
 *
 * Three layers: the .def file loader (ml_load), the matcher
 * that picks a primitive for each memory (mp_mmap), and the
 * full CLI pipeline that produces a real BRAM instance in
 * the FPGA JSON output. If any of these breaks, somebody
 * has either changed the schema, broken the cost model, or
 * silently dropped the BRAM emission path, and the rest of
 * the memory inference story rots from underneath them
 * before anybody downstream notices.
 */

#include "tharns.h"
#include "takahe.h"
#include <inttypes.h>

/* ---- Helper: write a small temp .def file ----
 * Returns 0 on success. Uses a hard-coded path so the test
 * harness can clean up. We do not care about race conditions
 * because the test runner is single-threaded. */

static int
sp_wdef(const char *path, const char *body)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fputs(body, fp);
    fclose(fp);
    return 0;
}

/* ============================================================ *
 * Loader tests                                                  *
 * ============================================================ */

/* ---- The real iCE40 def loads and reports two primitives ---- */

static void
ml_load_real(void)
{
    ml_lib_t lib;
    int rc;

    memset(&lib, 0, sizeof(lib));
    rc = ml_load(&lib, "defs/mems_ice40.def");
    CHEQ(rc, 0);
    CHEQ(lib.n_prim, 2);
    PASS();
}
TH_REG("mmap", ml_load_real)

/* ---- Missing files fail with non-zero return ---- */

static void
ml_missing(void)
{
    ml_lib_t lib;
    int rc;

    memset(&lib, 0, sizeof(lib));
    rc = ml_load(&lib, "defs/no_such_file.def");
    CHNE(rc, 0);
    CHEQ(lib.n_prim, 0);
    PASS();
}
TH_REG("mmap", ml_missing)

/* ---- SB_RAM40_4K parses with correct fields ---- */

static void
ml_bram_fields(void)
{
    ml_lib_t lib;
    const ml_prim_t *p;

    memset(&lib, 0, sizeof(lib));
    CHEQ(ml_load(&lib, "defs/mems_ice40.def"), 0);
    CHECK(lib.n_prim >= 1);

    p = &lib.prims[0];
    CHSTR(p->name, "SB_RAM40_4K");
    CHEQ(p->type, ML_TYPE_BRAM);
    CHEQ(p->abits, 11);
    CHEQ(p->size, (uint32_t)4096);
    CHEQ(p->n_widths, 4);
    CHEQ(p->widths[0], 2);
    CHEQ(p->widths[1], 4);
    CHEQ(p->widths[2], 8);
    CHEQ(p->widths[3], 16);
    CHEQ(p->cost, 100);
    CHEQ(p->n_ports, 2);
    PASS();
}
TH_REG("mmap", ml_bram_fields)

/* ---- SB_SPRAM256KA parses with correct fields, including
 * the higher cost that keeps it from beating BRAM ---- */

static void
ml_spram_fields(void)
{
    ml_lib_t lib;
    const ml_prim_t *p;

    memset(&lib, 0, sizeof(lib));
    CHEQ(ml_load(&lib, "defs/mems_ice40.def"), 0);
    CHECK(lib.n_prim >= 2);

    p = &lib.prims[1];
    CHSTR(p->name, "SB_SPRAM256KA");
    CHEQ(p->type, ML_TYPE_SPRAM);
    CHEQ(p->abits, 14);
    CHEQ(p->size, (uint32_t)262144);
    CHEQ(p->n_widths, 1);
    CHEQ(p->widths[0], 16);
    CHEQ(p->cost, 200);
    CHEQ(p->n_ports, 1);
    PASS();
}
TH_REG("mmap", ml_spram_fields)

/* ---- Port flags parse correctly ----
 * The BRAM has separate read and write ports; the write
 * port carries the byte-enable granularity flag. */

static void
ml_port_flags(void)
{
    ml_lib_t lib;
    const ml_prim_t *p;

    memset(&lib, 0, sizeof(lib));
    CHEQ(ml_load(&lib, "defs/mems_ice40.def"), 0);
    p = &lib.prims[0];

    CHEQ(p->ports[0].kind, ML_PORT_SR);
    CHEQ(p->ports[0].clk, 1);
    CHEQ(p->ports[0].has_clken, 1);
    CHEQ(p->ports[0].abswap, 3);

    CHEQ(p->ports[1].kind, ML_PORT_SW);
    CHEQ(p->ports[1].be_gran, 16);
    PASS();
}
TH_REG("mmap", ml_port_flags)

/* ---- Comments and blank lines are tolerated ---- */

static void
ml_comments(void)
{
    ml_lib_t lib;
    const char *body =
        "# Header comment\n"
        "\n"
        "mem TINY    # trailing comment after name\n"
        "  type   bram\n"
        "  abits  4\n"
        "  widths 1 8\n"
        "  size   128\n"
        "  init   none\n"
        "  cost   42\n"
        "  port sw W\n"
        "    clock posedge\n"
        "  end\n"
        "end\n";

    CHEQ(sp_wdef("tests/_tmp_mems.def", body), 0);
    memset(&lib, 0, sizeof(lib));
    CHEQ(ml_load(&lib, "tests/_tmp_mems.def"), 0);
    CHEQ(lib.n_prim, 1);
    CHSTR(lib.prims[0].name, "TINY");
    CHEQ(lib.prims[0].abits, 4);
    CHEQ(lib.prims[0].cost, 42);
    remove("tests/_tmp_mems.def");
    PASS();
}
TH_REG("mmap", ml_comments)

/* ============================================================ *
 * Matcher tests                                                 *
 * ============================================================ */

/* ---- 8x16 memory picks BRAM over SPRAM ----
 * Both fit, BRAM is cheaper. The whole point of flipping
 * the cost in the def file was to make this case go right. */

static void
mm_picks_bram(void)
{
    rt_mod_t M;
    ml_lib_t lib;
    int n;

    rt_init(&M, 64, 64);
    M.mems[0].data_w = 8;
    M.mems[0].depth = 16;
    M.mems[0].addr_w = 4;
    M.n_mem = 1;

    memset(&lib, 0, sizeof(lib));
    CHEQ(ml_load(&lib, "defs/mems_ice40.def"), 0);

    n = mp_mmap(&M, &lib);
    CHEQ(n, 1);
    CHEQ(M.mems[0].prim_set, 1);
    CHSTR(lib.prims[M.mems[0].prim_idx].name, "SB_RAM40_4K");

    rt_free(&M);
    PASS();
}
TH_REG("mmap", mm_picks_bram)

/* ---- Memory too wide gets left as soft logic ----
 * 64-bit data is wider than any width in either iCE40
 * primitive, so neither matches and prim_set stays zero. */

static void
mm_too_wide(void)
{
    rt_mod_t M;
    ml_lib_t lib;
    int n;

    rt_init(&M, 64, 64);
    M.mems[0].data_w = 64;
    M.mems[0].depth = 16;
    M.n_mem = 1;

    memset(&lib, 0, sizeof(lib));
    ml_load(&lib, "defs/mems_ice40.def");

    n = mp_mmap(&M, &lib);
    CHEQ(n, 0);
    CHEQ(M.mems[0].prim_set, 0);
    rt_free(&M);
    PASS();
}
TH_REG("mmap", mm_too_wide)

/* ---- Memory too deep falls through to SPRAM ----
 * 8-bit wide, 10000-deep is too deep for BRAM at 8-bit
 * width (max 512), but fits SPRAM (16384 at 16-bit width).
 * SPRAM wins by being the only one that fits. */

static void
mm_falls_to_spram(void)
{
    rt_mod_t M;
    ml_lib_t lib;
    int n;

    rt_init(&M, 64, 64);
    M.mems[0].data_w = 8;
    M.mems[0].depth = 10000;
    M.n_mem = 1;

    memset(&lib, 0, sizeof(lib));
    ml_load(&lib, "defs/mems_ice40.def");

    n = mp_mmap(&M, &lib);
    CHEQ(n, 1);
    CHEQ(M.mems[0].prim_set, 1);
    CHSTR(lib.prims[M.mems[0].prim_idx].name, "SB_SPRAM256KA");
    rt_free(&M);
    PASS();
}
TH_REG("mmap", mm_falls_to_spram)

/* ---- Memory too deep for everything stays soft ---- */

static void
mm_too_deep(void)
{
    rt_mod_t M;
    ml_lib_t lib;
    int n;

    rt_init(&M, 64, 64);
    M.mems[0].data_w = 8;
    M.mems[0].depth = 1000000;  /* a million entries */
    M.n_mem = 1;

    memset(&lib, 0, sizeof(lib));
    ml_load(&lib, "defs/mems_ice40.def");

    n = mp_mmap(&M, &lib);
    CHEQ(n, 0);
    CHEQ(M.mems[0].prim_set, 0);
    rt_free(&M);
    PASS();
}
TH_REG("mmap", mm_too_deep)

/* ---- Multiple memories in one module all map ---- */

static void
mm_multi(void)
{
    rt_mod_t M;
    ml_lib_t lib;
    int n;

    rt_init(&M, 64, 64);
    M.mems[0].data_w = 8;  M.mems[0].depth = 16;
    M.mems[1].data_w = 4;  M.mems[1].depth = 32;
    M.mems[2].data_w = 2;  M.mems[2].depth = 128;
    M.n_mem = 3;

    memset(&lib, 0, sizeof(lib));
    ml_load(&lib, "defs/mems_ice40.def");

    n = mp_mmap(&M, &lib);
    CHEQ(n, 3);
    CHEQ(M.mems[0].prim_set, 1);
    CHEQ(M.mems[1].prim_set, 1);
    CHEQ(M.mems[2].prim_set, 1);
    rt_free(&M);
    PASS();
}
TH_REG("mmap", mm_multi)

/* ============================================================ *
 * CLI integration tests                                         *
 * ============================================================ */

/* ---- sram.sv produces an SB_RAM40_4K instance in JSON ---- */

static void
cli_bram(void)
{
    char obuf[TH_BUFSZ];
    int rc;
    FILE *fp;
    char json[TH_BUFSZ];
    size_t nr;

    if (!th_exist("tests/sram.sv")) SKIP("no sram.sv");

    rc = th_run(TK_BIN " --fpga tests/out.json tests/sram.sv",
                obuf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(obuf, "memlib:") != NULL);
    CHECK(strstr(obuf, "SB_RAM40_4K") != NULL);
    CHECK(strstr(obuf, "BRAM instances emitted") != NULL);

    fp = fopen("tests/out.json", "r");
    CHECK(fp != NULL);
    nr = fread(json, 1, TH_BUFSZ - 1, fp);
    json[nr] = '\0';
    fclose(fp);

    CHECK(strstr(json, "SB_RAM40_4K") != NULL);
    CHECK(strstr(json, "READ_MODE") != NULL);
    CHECK(strstr(json, "RDATA") != NULL);
    CHECK(strstr(json, "WDATA") != NULL);

    remove("tests/out.json");
    PASS();
}
TH_REG("mmap", cli_bram)

/* ---- Non-memory design produces no BRAM instances ----
 * Smoke check that smoke.sv (counter + ALU, no arrays)
 * does not accidentally emit a BRAM instance. */

static void
cli_nobram(void)
{
    char obuf[TH_BUFSZ];
    int rc;
    FILE *fp;
    char json[TH_BUFSZ];
    size_t nr;

    if (!th_exist("tests/smoke.sv")) SKIP("no smoke.sv");

    rc = th_run(TK_BIN " --fpga tests/out.json tests/smoke.sv",
                obuf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(obuf, "BRAM instances emitted") == NULL);

    fp = fopen("tests/out.json", "r");
    CHECK(fp != NULL);
    nr = fread(json, 1, TH_BUFSZ - 1, fp);
    json[nr] = '\0';
    fclose(fp);

    CHECK(strstr(json, "SB_RAM40_4K") == NULL);

    remove("tests/out.json");
    PASS();
}
TH_REG("mmap", cli_nobram)

/* ---- BRAM emission honours --lang mi ----
 * Mapper print messages are stdout, not bilingual yet,
 * but the rest of the pipeline (catalogue load, error
 * paths) must still work alongside the mapper. */

static void
cli_lang(void)
{
    char obuf[TH_BUFSZ];
    int rc;

    if (!th_exist("tests/sram.sv")) SKIP("no sram.sv");

    rc = th_run(TK_BIN " --lang mi --fpga tests/out.json tests/sram.sv",
                obuf, TH_BUFSZ);
    CHEQ(rc, 0);
    CHECK(strstr(obuf, "SB_RAM40_4K") != NULL);
    CHECK(strstr(obuf, "lang: ") != NULL);
    remove("tests/out.json");
    PASS();
}
TH_REG("mmap", cli_lang)
