/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

/* tmain.c -- Takahe test runner
 * Because untested synthesis is just very expensive guessing. */

#include "tharns.h"

tcase_t th_list[TH_MAXTS];
int th_cnt = 0;
int npass  = 0;
int nfail  = 0;
int nskip  = 0;

int th_run(const char *cmd, char *obuf, int osz)
{
    char full[TH_BUFSZ];
    snprintf(full, TH_BUFSZ, "%s 2>&1", cmd);
    FILE *fp = popen(full, "r");
    if (!fp) { obuf[0] = '\0'; return -1; }
    int n = (int)fread(obuf, 1, (size_t)(osz - 1), fp);
    if (n < 0) n = 0;
    obuf[n] = '\0';

    /* Drain whatever is left. Closing the pipe while the child is still
     * writing hands it a SIGPIPE, and pclose then reports a failure that
     * belongs to the buffer size rather than to the command. Anything with
     * --parse in it outruns the buffer easily. */
    if (n == osz - 1) {
        char sink[4096];
        while (fread(sink, 1, sizeof sink, fp) > 0)
            ;
    }

    int rc = pclose(fp);
#ifndef _WIN32
    if (rc != -1 && (rc & 0xFF) == 0)
        rc = (rc >> 8) & 0xFF;
#endif

    /* Every caller wants zero, so a non-zero exit means the child said
     * something. Printing it is how a sanitiser report reaches the log. */
    if (rc != 0 && obuf[0] != '\0')
        printf("\n--- exited %d: %s ---\n%s---\n", rc, cmd, obuf);

    return rc;
}

int th_exist(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

/* Category first, then name. Registration runs through constructors, whose
 * order across translation units follows the link line, so without this the
 * output reshuffles whenever a test file is added and two runs will not
 * diff against each other. */
static int
th_cmp(const void *a, const void *b)
{
    const tcase_t *x = (const tcase_t *)a;
    const tcase_t *y = (const tcase_t *)b;
    int c = strcmp(x->tcats, y->tcats);

    return c != 0 ? c : strcmp(x->tname, y->tname);
}

int main(int argc, char **argv)
{
    int i;
    const char *filter = NULL;

    if (argc > 1 && strcmp(argv[1], "--all") != 0)
        filter = argv[1];

    if (th_cnt > 1)
        qsort(th_list, (size_t)th_cnt, sizeof th_list[0], th_cmp);

    printf("\nTakahe Test Suite\n");
    printf("=================\n");

    for (i = 0; i < th_cnt; i++) {
        if (filter && strcmp(filter, th_list[i].tcats) != 0)
            continue;

        int fbefore = nfail, sbefore = nskip;
        int before = npass + nfail + nskip;
        printf("  %-28s", th_list[i].tname);
        fflush(stdout);

        th_list[i].func();

        int after = npass + nfail + nskip;
        if (after == before) {
            /* Asserted nothing and did not crash. Counted as a pass, which
             * is generous, but a gutted test body is the caller's problem. */
            npass++;
        }
        /* Only when it actually passed. The old condition was true whenever
         * any counter moved, so a failing test printed its FAIL line and then
         * PASS underneath it. */
        if (nfail == fbefore && nskip == sbefore)
            printf("PASS\n");
    }

    printf("=================\n");
    printf("%d tests: %d passed, %d failed, %d skipped\n\n",
           npass + nfail + nskip, npass, nfail, nskip);

    return nfail > 0 ? 1 : 0;
}
