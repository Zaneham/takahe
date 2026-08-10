/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tjed.c -- JEDEC fuse map reader tests
 * Checksums in these fixtures were worked out by hand from JESD3-C,
 * not by asking jd_read what it thought they were. */

#include "tharns.h"
#include "takahe.h"

#define JD_TMP "tjed_tmp.jed"

/* 32 fuses. Bytes come out ED 00 0F 00, so the fuse checksum is 00FC. */
static const char jd_good[] =
    "\x02" "GAL16V8 test*\n"
    "QF32*\n"
    "QP20*\n"
    "F0*\n"
    "L0000 10110111*\n"
    "L0016 11110000*\n"
    "C00FC*\n"
    "\x03" "0E24\n";

static int jd_write(const char *text)
{
    FILE *fp = fopen(JD_TMP, "wb");
    size_t n;

    if (fp == NULL) return -1;
    n = fwrite(text, 1, strlen(text), fp);
    fclose(fp);
    return n == strlen(text) ? 0 : -1;
}

/* ---- jd_ok: a well-formed file ---- */

static void jd_ok(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    CHECK(J != NULL);
    CHECK(jd_write(jd_good) == 0);

    CHEQ(jd_read(J, JD_TMP), 0);
    CHEQ(J->n_err, 0u);
    CHEQ(J->qf, 32u);
    CHEQ(J->qp, 20u);
    CHSTR(J->devid, "GAL16V8 test");

    CHEQ(J->csum, 0x00FC);
    CHEQ(J->ccalc, 0x00FC);
    CHEQ(J->tsum, 0x0E24);
    CHEQ(J->tcalc, 0x0E24);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_ok)

/* ---- jd_bits: the fuses land where the L fields put them ---- */

static void jd_bits(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    const char *want = "10110111";
    int i;

    CHECK(J != NULL);
    CHECK(jd_write(jd_good) == 0);
    CHEQ(jd_read(J, JD_TMP), 0);

    for (i = 0; i < 8; i++)
        CHEQ(jd_getf(J, (uint32_t)i), want[i] == '1');

    /* Nothing addressed 8 to 15, so F0 fills them. */
    for (i = 8; i < 16; i++)
        CHEQ(jd_getf(J, (uint32_t)i), 0);

    for (i = 16; i < 20; i++) CHEQ(jd_getf(J, (uint32_t)i), 1);
    for (i = 20; i < 32; i++) CHEQ(jd_getf(J, (uint32_t)i), 0);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_bits)

/* ---- jd_badc: a wrong fuse checksum is reported, not swallowed ---- */

static void jd_badc(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    CHECK(J != NULL);

    CHECK(jd_write(
        "\x02" "bad*\n" "QF32*\n" "F0*\n"
        "L0000 10110111*\n"
        "C1234*\n"
        "\x03" "0000\n") == 0);

    CHEQ(jd_read(J, JD_TMP), -1);
    CHECK(J->n_err > 0);
    CHEQ(J->csum, 0x1234);
    CHNE(J->ccalc, 0x1234);

    /* Still readable, which is the whole point of not bailing out. */
    CHEQ(jd_getf(J, 0), 1);
    CHEQ(jd_getf(J, 1), 0);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_badc)

/* ---- jd_zerc: 0000 means do not check ---- */

static void jd_zerc(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    CHECK(J != NULL);

    CHECK(jd_write(
        "\x02" "nocheck*\n" "QF32*\n" "F1*\n"
        "C0000*\n"
        "\x03" "0000\n") == 0);

    CHEQ(jd_read(J, JD_TMP), 0);
    CHEQ(J->n_err, 0u);

    /* F1 with no L field anywhere, so every fuse is blown. */
    CHEQ(jd_getf(J, 0), 1);
    CHEQ(jd_getf(J, 31), 1);
    CHEQ(J->ccalc, 0x03FCu);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_zerc)

/* ---- jd_ford: F after L still only fills what L missed ---- */

static void jd_ford(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    int i;

    CHECK(J != NULL);

    CHECK(jd_write(
        "\x02" "order*\n" "QF16*\n"
        "L0000 00000000*\n"
        "F1*\n"
        "C0000*\n"
        "\x03" "0000\n") == 0);

    CHEQ(jd_read(J, JD_TMP), 0);
    for (i = 0; i < 8; i++)  CHEQ(jd_getf(J, (uint32_t)i), 0);
    for (i = 8; i < 16; i++) CHEQ(jd_getf(J, (uint32_t)i), 1);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_ford)

/* ---- jd_noqf: a file with no fuse count is refused ---- */

static void jd_noqf(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    CHECK(J != NULL);

    CHECK(jd_write("\x02" "nothing*\n" "L0000 1010*\n" "\x03" "0000\n") == 0);

    CHEQ(jd_read(J, JD_TMP), -1);
    CHECK(J->n_err > 0);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_noqf)

/* ---- jd_nostx: not a JEDEC file at all ---- */

static void jd_nostx(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    CHECK(J != NULL);

    CHECK(jd_write("module m; endmodule\n") == 0);
    CHEQ(jd_read(J, JD_TMP), -1);
    CHECK(J->n_err > 0);

    remove(JD_TMP);
    free(J);
    PASS();
}
TH_REG("jed", jd_nostx)

/* ---- jd_nofil: a missing file is an error, not a crash ---- */

static void jd_nofil(void)
{
    jd_file_t *J = (jd_file_t *)calloc(1, sizeof(jd_file_t));
    CHECK(J != NULL);

    CHEQ(jd_read(J, "tjed_does_not_exist.jed"), -1);
    CHECK(J->n_err > 0);

    free(J);
    PASS();
}
TH_REG("jed", jd_nofil)
