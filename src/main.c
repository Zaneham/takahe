/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * main.c -- Takahe CLI entry point
 *
 * Usage: takahe [flags] <source.sv>
 *
 * The front door to open source chip design.
 * Please wipe your feet and leave your Synopsys
 * license at the door.
 */

#include "takahe.h"
#include <inttypes.h>

#define TK_MAX_SRC (16u * 1024u * 1024u)  /* 16MB source limit */

static void
usage(const char *prog)
{
    printf("takahe v0.1.0 -- open-source hardware synthesis\n\n");
    printf("usage: %s [flags] <source.sv|.vhd>\n\n", prog);
    printf("languages:\n");
    printf("  (default)   SystemVerilog (IEEE 1800-2017)\n");
    printf("  --vhdl      VHDL (IEEE 1076-2008)\n\n");
    printf("synthesis:\n");
    printf("  --lex       dump tokens\n");
    printf("  --parse     dump AST + RTL IR\n");
    printf("  --opt       optimise (constant propagation + DCE)\n");
    printf("  --equiv     equivalence check (pre-opt vs post-opt)\n");
    printf("  --lib <f>   Liberty .lib cell library for technology mapping\n");
    printf("  --map <f>   emit mapped gate-level Verilog\n\n");
    printf("output formats:\n");
    printf("  --blif <f>  emit BLIF netlist\n");
    printf("  --yosys <f> emit Yosys JSON netlist\n\n");
    printf("options:\n");
    printf("  --radix <n> digit radix (2=binary, 3=ternary, 12=dozenal)\n");
    printf("  --sta <mhz> static timing analysis at target frequency\n");
    printf("  --defs <f>  token definition file (default: defs/sv_tok.def)\n");
    printf("  --lang <en|mi> message language\n");
    printf("  --help      this message\n\n");
    printf("supported PDKs: SKY130, IHP SG13G2, GF180MCU, ASAP7\n");
    printf("because chip design shouldn't cost more than a house.\n");
}

int
main(int argc, char **argv)
{
    tk_lex_t *L;
    const char *src_path = NULL;
    const char *def_path = "defs/sv_tok.def";
    int mode_lex = 0;
    int mode_parse = 0;
    int mode_opt = 0;
    const char *blif_path = NULL;
    const char *yosys_path = NULL;
    const char *lib_path = NULL;
    const char *map_path = NULL;
    int mode_vhdl = 0;
    int mode_equiv = 0;
    int radix = TK_RADIX_BIN;
    int sta_mhz = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--lex") == 0) {
            mode_lex = 1;
        } else if (strcmp(argv[i], "--parse") == 0) {
            mode_parse = 1;
        } else if (strcmp(argv[i], "--opt") == 0) {
            mode_opt = 1;
            mode_parse = 1;  /* --opt implies --parse pipeline */
        } else if (strcmp(argv[i], "--blif") == 0 && i + 1 < argc) {
            blif_path = argv[++i];
            mode_opt = 1;
            mode_parse = 1;
        } else if (strcmp(argv[i], "--yosys") == 0 && i + 1 < argc) {
            yosys_path = argv[++i];
            mode_opt = 1;
            mode_parse = 1;
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            lib_path = argv[++i];
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
            mode_opt = 1;
            mode_parse = 1;
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "mi") == 0) tk_slang(1);
        } else if (strcmp(argv[i], "--radix") == 0 && i + 1 < argc) {
            radix = atoi(argv[++i]);
            mode_parse = 1;
        } else if (strcmp(argv[i], "--sta") == 0 && i + 1 < argc) {
            sta_mhz = atoi(argv[++i]);
            mode_opt = 1;
            mode_parse = 1;
        } else if (strcmp(argv[i], "--equiv") == 0) {
            mode_equiv = 1;
            mode_opt = 1;
            mode_parse = 1;
        } else if (strcmp(argv[i], "--vhdl") == 0) {
            mode_vhdl = 1;
            def_path = "defs/vhdl_tok.def";
        } else if (strcmp(argv[i], "--defs") == 0 && i + 1 < argc) {
            def_path = argv[++i];
        } else if (argv[i][0] != '-') {
            src_path = argv[i];
        } else {
            fprintf(stderr, "takahe: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!src_path) {
        fprintf(stderr, "takahe: no input file\n");
        usage(argv[0]);
        return 1;
    }

    /* Allocate lexer context on heap (it's large) */
    L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    if (!L) {
        fprintf(stderr, "takahe: failed to allocate lexer\n");
        return 1;
    }

    /* Load definitions */
    if (tk_ldinit(L, def_path) != 0) {
        free(L);
        return 1;
    }

    /* Read source file */
    {
        FILE *fp = fopen(src_path, "r");
        long fsz;
        char *buf;
        size_t nr;

        if (!fp) {
            fprintf(stderr, "takahe: cannot open '%s'\n", src_path);
            tk_ldfree(L);
            free(L);
            return 1;
        }

        fseek(fp, 0, SEEK_END);
        fsz = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (fsz <= 0 || (uint32_t)fsz > TK_MAX_SRC) {
            fprintf(stderr, "takahe: file too large or empty (%ld bytes)\n", fsz);
            fclose(fp);
            tk_ldfree(L);
            free(L);
            return 1;
        }

        buf = (char *)malloc((size_t)fsz + 1);
        if (!buf) {
            fprintf(stderr, "takahe: malloc failed\n");
            fclose(fp);
            tk_ldfree(L);
            free(L);
            return 1;
        }

        nr = fread(buf, 1, (size_t)fsz, fp);
        fclose(fp);
        buf[nr] = '\0';

        printf("takahe: read %u bytes from %s\n", (unsigned)nr, src_path);
        fflush(stdout);

        /* Preprocess */
        uint32_t pp_max = (uint32_t)nr + (uint32_t)nr + 4096u;
        char *ppbuf = (char *)malloc((size_t)pp_max);
        if (!ppbuf) {
            fprintf(stderr, "takahe: malloc failed for preprocessor\n");
            free(buf);
            tk_ldfree(L);
            free(L);
            return 1;
        }
        uint32_t pp_len = 0;
        tk_preproc(buf, (uint32_t)nr, ppbuf, pp_max,
                   &pp_len, NULL, 0);
        printf("takahe: preprocessed %u -> %u bytes\n",
               (unsigned)nr, (unsigned)pp_len);

        /* Lex */
        printf("takahe: lexing %s (%u bytes)\n", src_path, (unsigned)pp_len);
        if (mode_vhdl)
            vh_lex(L, ppbuf, pp_len);
        else
            tk_lex(L, ppbuf, pp_len);

        printf("takahe: %u tokens, %u errors\n", L->n_tok, L->n_err);

        /* Dump tokens */
        if (mode_lex) {
            uint32_t t;
            for (t = 0; t < L->n_tok; t++) {
                tk_token_t *tok = &L->tokens[t];
                const char *text = L->strs + tok->off;
                const char *tname = tk_tokstr(tok->type);

                printf("  %4u:%-3u  %-8s", tok->line, tok->col, tname);

                if (tok->type == TK_TOK_KWD) {
                    printf("  %-20s  [kwd #%u]",
                           text, (unsigned)tok->sub);
                } else if (tok->type == TK_TOK_OP) {
                    printf("  %-20s  [op #%u %s]",
                           text, (unsigned)tok->sub,
                           tk_opstr(L, tok->sub));
                } else if (tok->type == TK_TOK_EOF) {
                    printf("  (eof)");
                } else {
                    printf("  %.*s", tok->len > 40 ? 40 : tok->len, text);
                }
                printf("\n");
            }
        }

        /* Parse */
        if (mode_parse) {
            tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
            if (!P) {
                fprintf(stderr, "takahe: failed to allocate parser\n");
                free(buf);
                tk_ldfree(L);
                free(L);
                return 1;
            }

            if ((mode_vhdl ? vh_pinit(P, L) : tk_pinit(P, L)) != 0) {
                fprintf(stderr, "takahe: parser init failed\n");
                free(P);
                free(buf);
                tk_ldfree(L);
                free(L);
                return 1;
            }

            if (mode_vhdl)
                vh_parse(P);
            else
                tk_parse(P);
            printf("takahe: %u AST nodes, %u errors\n",
                   P->n_node, P->n_err);

            if (P->n_err > 0) {
                uint32_t e;
                for (e = 0; e < P->n_err; e++) {
                    fprintf(stderr, "  %u:%u: %s\n",
                            P->errors[e].line, P->errors[e].col,
                            P->errors[e].msg);
                }
            }

            /* Constant evaluation */
            ce_val_t *cvals = (ce_val_t *)calloc(P->n_node, sizeof(ce_val_t));
            if (cvals) {
                int neval = ce_eval(P, cvals, P->n_node);
                printf("takahe: %d constants evaluated\n", neval);

                /* Show evaluated constants */
                {
                    uint32_t ci;
                    for (ci = 1; ci < P->n_node; ci++) {
                        if (cvals[ci].valid &&
                            P->nodes[ci].type == TK_AST_INT_LIT) {
                            /* Skip raw literals, only show computed */
                        }
                        if (cvals[ci].valid &&
                            (P->nodes[ci].type == TK_AST_BINARY_OP ||
                             P->nodes[ci].type == TK_AST_UNARY_OP ||
                             P->nodes[ci].type == TK_AST_CALL)) {
                            printf("  node %u: %" PRId64 " (w=%u)\n",
                                   ci, cvals[ci].val, cvals[ci].width);
                        }
                    }
                }
                /* Elaboration: resolve params, substitute, eval generates */
                el_elab(P, cvals, P->n_node);

                /* Generate expansion: prune dead branches */
                ge_expand(P);

                /* Hierarchy flattening: annotate instances */
                fl_flat(P);

                /* Width inference */
                {
                    wi_val_t *wvals = (wi_val_t *)calloc(P->n_node,
                                                          sizeof(wi_val_t));
                    if (wvals) {
                        int nw = wi_eval(P, cvals, P->n_node,
                                         wvals, P->n_node);
                        printf("takahe: %d widths resolved\n", nw);

                        /* Show port/net widths */
                        {
                            uint32_t wi;
                            for (wi = 1; wi < P->n_node; wi++) {
                                if (!wvals[wi].resolved) continue;
                                tk_ast_type_t t = P->nodes[wi].type;
                                if (t == TK_AST_PORT ||
                                    t == TK_AST_NET_DECL ||
                                    t == TK_AST_PARAM ||
                                    t == TK_AST_LOCALPARAM) {
                                    const char *nm = "";
                                    uint32_t ch = P->nodes[wi].first_child;
                                    KA_GUARD(gw, 10);
                                    while (ch && gw--) {
                                        if (P->nodes[ch].type == TK_AST_IDENT) {
                                            nm = P->lex->strs +
                                                 P->nodes[ch].d.text.off;
                                            break;
                                        }
                                        ch = P->nodes[ch].next_sib;
                                    }
                                    printf("  %s: %u bits\n",
                                           nm, wvals[wi].width);
                                }
                            }
                        }
                        /* RTL Lowering */
                        {
                            rt_mod_t *rtl;
                            if (radix != TK_RADIX_BIN) {
                                rtl = lw_build_r(P, cvals, wvals,
                                                 P->n_node, (uint8_t)radix);
                                printf("takahe: radix %d synthesis\n", radix);
                            } else {
                                rtl = lw_build(P, cvals, wvals, P->n_node);
                            }
                            /* Load cell defs — pick file by radix */
                            cd_lib_t *cdlib = NULL;
                            {
                                cd_lib_t *cl = (cd_lib_t *)calloc(1, sizeof(cd_lib_t));
                                const char *cdf = "defs/cells.def";
                                if (radix == 3) cdf = "defs/cells_ter.def";
                                else if (radix == 4) cdf = "defs/cells_dna.def";
                                else if (radix == 7) cdf = "defs/cells_epist.def";
                                else if (radix == 8) cdf = "defs/cells_iching.def";
                                else if (radix == 12) cdf = "defs/cells_doz.def";
                                if (cl) {
                                    if (cd_load(cl, cdf) == 0)
                                        cdlib = cl;
                                    else
                                        free(cl);
                                }
                            }
                            if (rtl) {
                                /* Save pre-opt copy for equiv check */
                                rt_mod_t *rtl_pre = NULL;
                                if (mode_equiv) {
                                    rtl_pre = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
                                    if (rtl_pre) {
                                        memcpy(rtl_pre, rtl, sizeof(rt_mod_t));
                                        /* Deep copy nets/cells/strs */
                                        rtl_pre->nets = (rt_net_t *)malloc(
                                            rtl->max_net * sizeof(rt_net_t));
                                        rtl_pre->cells = (rt_cell_t *)malloc(
                                            rtl->max_cell * sizeof(rt_cell_t));
                                        rtl_pre->strs = (char *)malloc(rtl->str_max);
                                        if (rtl_pre->nets && rtl_pre->cells && rtl_pre->strs) {
                                            memcpy(rtl_pre->nets, rtl->nets,
                                                   rtl->n_net * sizeof(rt_net_t));
                                            memcpy(rtl_pre->cells, rtl->cells,
                                                   rtl->n_cell * sizeof(rt_cell_t));
                                            memcpy(rtl_pre->strs, rtl->strs, rtl->str_len);
                                        } else {
                                            free(rtl_pre->nets); free(rtl_pre->cells);
                                            free(rtl_pre->strs); free(rtl_pre);
                                            rtl_pre = NULL;
                                        }
                                    }
                                }

                                if (mode_opt) {
                                    int nopt = op_opt(rtl, cdlib);
                                    printf("takahe: %d optimisations\n",
                                           nopt);
                                }

                                /* Equivalence check: pre-opt vs post-opt */
                                if (mode_equiv && rtl_pre) {
                                    eq_check(rtl_pre, rtl);
                                    rt_free(rtl_pre);
                                    free(rtl_pre);
                                }
                                if (blif_path) {
                                    FILE *bf = fopen(blif_path, "w");
                                    if (bf) {
                                        mp_blif(rtl, bf);
                                        fclose(bf);
                                        printf("takahe: wrote %s\n",
                                               blif_path);
                                    } else {
                                        fprintf(stderr,
                                            "takahe: cannot write '%s'\n",
                                            blif_path);
                                    }
                                }
                                if (yosys_path) {
                                    FILE *yf = fopen(yosys_path, "w");
                                    if (yf) {
                                        mp_yosys(rtl, yf);
                                        fclose(yf);
                                        printf("takahe: wrote %s\n",
                                               yosys_path);
                                    } else {
                                        fprintf(stderr,
                                            "takahe: cannot write '%s'\n",
                                            yosys_path);
                                    }
                                }
                                if (map_path) {
                                    if (!lib_path) {
                                        fprintf(stderr,
                                            "takahe: --map requires --lib\n");
                                    } else {
                                        lb_lib_t *llib = (lb_lib_t *)calloc(1, sizeof(lb_lib_t));
                                        if (llib && lb_load(llib, lib_path) == 0) {
                                            mp_bind_t btbl[RT_CELL_COUNT];
                                            mp_bind(llib, btbl);
                                            mp_bblst(rtl);
                                            /* Post-blast optimisation: cprop folds
                                             * the thousands of new CONST cells,
                                             * pmatch merges NOT+AND→NAND, DCE
                                             * removes dead intermediates. */
                                            {
                                                int popt = op_opt(rtl, cdlib);
                                                printf("takahe: %d post-blast optimisations\n", popt);
                                            }
                                            /* Espresso: minimise small logic cones.
                                             * Each cone is individually journaled —
                                             * if a rebuild fails, that cone rolls
                                             * back. CICS pattern. */
                                            {
                                                uint32_t pre_cells = rtl->n_cell;
                                                jr_begin(rtl);
                                                op_espro(rtl);
                                                /* Did Espresso help or hurt? */
                                                if (rtl->n_cell > pre_cells + 1000) {
                                                    /* Net increase > 1000 cells = something
                                                     * went wrong. Roll back everything. */
                                                    jr_rback(rtl, NULL);
                                                    printf("takahe: espresso rolled back"
                                                           " (cell count increased)\n");
                                                } else {
                                                    jr_commit();
                                                }
                                            }
                                            /* Clean up after Espresso */
                                            op_opt(rtl, cdlib);
                                            {
                                                FILE *mf = fopen(map_path, "w");
                                                if (mf) {
                                                    em_vlog(rtl, llib, btbl, mf);
                                                    fclose(mf);
                                                    printf("takahe: wrote %s\n",
                                                           map_path);
                                                } else {
                                                    fprintf(stderr,
                                                        "takahe: cannot write '%s'\n",
                                                        map_path);
                                                }
                                            }
                                            /* Area report */
                                            {
                                                uint32_t ci3;
                                                double area = 0.0;
                                                uint32_t gcnt = 0;
                                                for (ci3 = 1; ci3 < rtl->n_cell; ci3++) {
                                                    rt_ctype_t ct2 = rtl->cells[ci3].type;
                                                    if (ct2 == RT_CELL_COUNT) continue;
                                                    gcnt++;
                                                    if (btbl[ct2].valid)
                                                        area += (double)llib->cells[btbl[ct2].cell_idx].area;
                                                }
                                                printf("takahe: %u gates, %.0f um2 area\n",
                                                       gcnt, area);
                                            }
                                            /* Timing-driven optimisation.
                                             * Journaled: if resizing makes area
                                             * explode without fixing timing,
                                             * roll back the binding changes. */
                                            if (sta_mhz > 0) {
                                                tk_fs_t tclk = 1000000000LL
                                                    / (tk_fs_t)sta_mhz;
                                                jr_begin(rtl);
                                                op_tdopt(rtl, llib, btbl, tclk);
                                                jr_commit();
                                            }
                                            /* STA */
                                            if (sta_mhz > 0) {
                                                /* clk period = 10^9 ns / freq_mhz,
                                                 * in fs = 10^15 / (freq * 10^6) = 10^9 / freq */
                                                /* period = 10^9 / freq_mhz (in fs) */
                                                tk_fs_t clk_fs = 1000000000LL
                                                                 / (tk_fs_t)sta_mhz;
                                                ta_sta(rtl, llib, btbl, clk_fs);
                                            }
                                        }
                                        free(llib);
                                    }
                                }
                                rt_dump(rtl);
                                rt_free(rtl);
                                free(rtl);
                            }
                            free(cdlib);
                        }

                        free(wvals);
                    }
                }

                free(cvals);
            }

            /* Dump AST */
            printf("\n--- AST ---\n");
            tk_pdump(P, 1, 0);  /* node 1 = root */

            tk_pfree(P);
            free(P);
        }

        free(ppbuf);
        free(buf);
    }

    tk_ldfree(L);
    free(L);

    return 0;
}
