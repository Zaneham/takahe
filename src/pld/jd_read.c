/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * jd_read.c -- JEDEC fuse map reader for Takahe
 *
 * Reads the fuse states out of a JESD3-C file as written by PLD
 * assemblers and device programmers. Test vectors are skipped.
 *
 * A file that fails a checksum is still returned with the mismatch
 * recorded, because that is nearly always a truncated download and
 * the fuses are worth looking at anyway.
 *
 * JEDEC JESD3-C, Standard Data Transfer Format Between Data
 * Preparation System and Programmable Logic Device Programmer.
 */

#include "takahe.h"

#define JD_STX  0x02
#define JD_ETX  0x03

typedef struct {
    FILE     *fp;
    uint32_t  line;
    uint32_t  tsum;   /* running transmission checksum */
    int       acc;    /* accumulate once STX has been seen */
} jd_ctx_t;

/* ---- Fuse bits ---- */

static void
jd_set(jd_file_t *J, uint32_t n, int val)
{
    uint8_t bit;

    if (n >= JD_MAX_FUSE) return;
    bit = (uint8_t)(1u << (n & 7u));

    if (val) J->fuse[n >> 3] = (uint8_t)(J->fuse[n >> 3] | bit);
    else     J->fuse[n >> 3] = (uint8_t)(J->fuse[n >> 3] & ~bit);
    J->seen[n >> 3] = (uint8_t)(J->seen[n >> 3] | bit);
}

int
jd_getf(const jd_file_t *J, uint32_t n)
{
    if (n >= JD_MAX_FUSE) return 0;
    return (J->fuse[n >> 3] >> (n & 7u)) & 1;
}

static int
jd_wasset(const jd_file_t *J, uint32_t n)
{
    if (n >= JD_MAX_FUSE) return 0;
    return (J->seen[n >> 3] >> (n & 7u)) & 1;
}

/* Fuse 0 is the LSB of word 0, and the tail of the last word is already
 * zero because nothing above qf was ever set. */
static uint16_t
jd_fsum(const jd_file_t *J)
{
    uint32_t sum = 0, i, nb;

    nb = (J->qf + 7u) / 8u;
    if (nb > sizeof J->fuse) nb = (uint32_t)sizeof J->fuse;

    for (i = 0; i < nb; i++) sum += J->fuse[i];
    return (uint16_t)(sum & 0xFFFFu);
}

/* ---- Diagnostics ---- */

static void
jd_err(jd_file_t *J, uint32_t line, const char *msg)
{
    tk_err_t *e;
    size_t i;

    if (J->n_err >= JD_MAX_ERRS) return;
    e = &J->errors[J->n_err++];
    e->line = line;
    e->col  = 0;

    for (i = 0; i + 1 < sizeof e->msg && msg[i] != '\0'; i++)
        e->msg[i] = msg[i];
    e->msg[i] = '\0';
}

/* ---- Character source ---- */

static int
jd_gc(jd_ctx_t *C)
{
    int c = getc(C->fp);

    if (c == EOF) return EOF;
    if (C->acc) C->tsum += (uint32_t)(unsigned char)c;
    if (c == '\n') C->line++;
    return c;
}

static int
jd_isws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int
jd_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Consumes digits and hands back the character that stopped it. Saturates
 * rather than wrapping, and the caller range-checks against qf. */
static uint32_t
jd_num(jd_ctx_t *C, int *stop)
{
    uint32_t v = 0;
    int c;

    for (;;) {
        c = jd_gc(C);
        if (c < '0' || c > '9') break;
        if (v > 0xFFFFFFFFu / 10u) continue;
        v = v * 10u + (uint32_t)(c - '0');
    }
    *stop = c;
    return v;
}

/* Runs to the field terminator. Returns 1 if ETX arrived first. */
static int
jd_skip(jd_ctx_t *C)
{
    uint32_t guard = 0;
    int c;

    while ((c = jd_gc(C)) != EOF) {
        if (c == '*')     return 0;
        if (c == JD_ETX)  return 1;
        if (++guard > JD_MAX_FIELD) return -1;
    }
    return -1;
}

/* ---- Fields ---- */

static void
jd_lfld(jd_ctx_t *C, jd_file_t *J)
{
    uint32_t addr, guard = 0;
    int c;

    addr = jd_num(C, &c);

    for (;;) {
        if (c == '*' || c == EOF || c == JD_ETX) return;
        if (c == '0' || c == '1') {
            if (addr >= JD_MAX_FUSE) {
                jd_err(J, C->line, "fuse address beyond JD_MAX_FUSE");
                return;
            }
            jd_set(J, addr++, c == '1');
        } else if (!jd_isws(c)) {
            jd_err(J, C->line, "non-binary digit in an L field");
            return;
        }
        if (++guard > JD_MAX_FIELD) {
            jd_err(J, C->line, "unterminated L field");
            return;
        }
        c = jd_gc(C);
    }
}

static void
jd_qfld(jd_ctx_t *C, jd_file_t *J)
{
    uint32_t v;
    int sub, c;

    sub = jd_gc(C);
    v   = jd_num(C, &c);

    switch (sub) {
    case 'F': J->qf = v; break;
    case 'P': J->qp = v; break;
    case 'V': J->qv = v; break;
    default:
        jd_err(J, C->line, "unknown Q subfield");
        break;
    }
    if (c != '*') (void)jd_skip(C);
}

/* Four hex digits, per the standard. */
static void
jd_cfld(jd_ctx_t *C, jd_file_t *J)
{
    uint32_t v = 0;
    int i, c, d;

    for (i = 0; i < 4; i++) {
        c = jd_gc(C);
        d = jd_hex(c);
        if (d < 0) {
            jd_err(J, C->line, "malformed fuse checksum");
            if (c != '*') (void)jd_skip(C);
            return;
        }
        v = (v << 4) | (uint32_t)d;
    }
    J->csum = (uint16_t)v;
    J->has_c = 1;
    (void)jd_skip(C);
}

/* The design specification text, between STX and the first terminator.
 * Whatever a programmer put there is the closest thing to a device name. */
static void
jd_hdr(jd_ctx_t *C, jd_file_t *J)
{
    uint32_t guard = 0;
    size_t n = 0;
    int c, gap = 0;

    while ((c = jd_gc(C)) != EOF) {
        if (c == '*') break;
        if (++guard > JD_MAX_FIELD) break;

        if (jd_isws(c)) { gap = (n > 0); continue; }
        if (gap && n + 1 < sizeof J->devid) J->devid[n++] = ' ';
        gap = 0;
        if (n + 1 < sizeof J->devid) J->devid[n++] = (char)c;
    }
    J->devid[n] = '\0';
}

/* ---- Reader ---- */

int
jd_read(jd_file_t *J, const char *path)
{
    jd_ctx_t C;
    uint32_t i, guard = 0;
    int c, done = 0, rc = 0;

    memset(J, 0, sizeof *J);
    J->secur = -1;
    J->dflt  = 0xFF;          /* no F field seen yet */

    C.fp = fopen(path, "rb");
    if (C.fp == NULL) {
        jd_err(J, 0, "cannot open the JEDEC file");
        return -1;
    }
    C.line = 1;
    C.tsum = 0;
    C.acc  = 0;

    /* Anything ahead of STX is preamble and is outside the checksum. */
    while ((c = getc(C.fp)) != EOF && c != JD_STX)
        if (c == '\n') C.line++;

    if (c == EOF) {
        jd_err(J, C.line, "no STX, so this is not a JEDEC file");
        fclose(C.fp);
        return -1;
    }
    C.acc   = 1;
    C.tsum += JD_STX;

    jd_hdr(&C, J);

    while (!done) {
        if (++guard > JD_MAX_FIELD) {
            jd_err(J, C.line, "runaway field count");
            break;
        }

        do { c = jd_gc(&C); } while (jd_isws(c));

        if (c == EOF)    { jd_err(J, C.line, "end of file before ETX"); break; }
        if (c == JD_ETX) break;

        switch (c) {
        case 'L': jd_lfld(&C, J); break;
        case 'Q': jd_qfld(&C, J); break;
        case 'C': jd_cfld(&C, J); break;

        case 'F':
        case 'G': {
            int v = jd_gc(&C);
            if (v != '0' && v != '1') {
                jd_err(J, C.line, "F and G take 0 or 1");
            } else if (c == 'F') {
                J->dflt = (uint8_t)(v == '1');
            } else {
                J->secur = (int8_t)(v == '1');
            }
            done = (jd_skip(&C) == 1);
            break;
        }

        default:
            /* N, V, A, D, E, J, P, R, S, T, U, X and anything reserved. */
            done = (jd_skip(&C) == 1);
            break;
        }
    }

    /* The transmission checksum covers STX through ETX and stops there. */
    C.acc = 0;
    {
        uint32_t v = 0;
        int d, n = 0;

        while (n < 4 && (c = jd_gc(&C)) != EOF) {
            if (jd_isws(c)) continue;
            d = jd_hex(c);
            if (d < 0) break;
            v = (v << 4) | (uint32_t)d;
            n++;
        }
        if (n == 4) { J->tsum = (uint16_t)v; J->has_t = 1; }
    }
    fclose(C.fp);

    if (J->qf == 0) {
        jd_err(J, C.line, "no QF field, so the fuse count is unknown");
        return -1;
    }
    if (J->qf > JD_MAX_FUSE) {
        jd_err(J, C.line, "device is larger than JD_MAX_FUSE");
        return -1;
    }

    /* F applies to whatever no L field mentioned, whichever order they came
     * in. Absent, the standard leaves it undefined and we leave it at 0. */
    if (J->dflt != 0xFF)
        for (i = 0; i < J->qf; i++)
            if (!jd_wasset(J, i)) jd_set(J, i, J->dflt);

    J->ccalc = jd_fsum(J);
    J->tcalc = (uint16_t)(C.tsum & 0xFFFFu);

    if (J->has_c && J->csum != 0 && J->csum != J->ccalc) {
        jd_err(J, 0, "fuse checksum does not match the fuses");
        rc = -1;
    }
    /* Zero is the documented way to say do not check this. */
    if (J->has_t && J->tsum != 0 && J->tsum != J->tcalc) {
        jd_err(J, 0, "transmission checksum does not match the file");
        rc = -1;
    }
    return J->n_err > 0 ? -1 : rc;
}
