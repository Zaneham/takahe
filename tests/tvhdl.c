/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* tvhdl.c -- VHDL frontend tests
 * The US DoD tested their hardware descriptions with
 * billion-dollar weapons systems. We test ours with
 * string literals and asserts. Same energy, less shrapnel. */

#include "tharns.h"
#include "takahe.h"

/* ---- Helper: lex VHDL source ---- */

static int vh_lex_str(const char *src, tk_lex_t *L)
{
    uint32_t slen = (uint32_t)strlen(src);
    vh_lex(L, src, slen);
    return (int)L->n_tok;
}

/* ---- Helper: full VHDL pipeline from string ---- */

static rt_mod_t *
vh_rtl(const char *src)
{
    tk_lex_t *L;
    tk_parse_t *P;
    ce_val_t *cv;
    wi_val_t *wv;
    rt_mod_t *M;
    uint32_t slen;

    L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    if (!L || !P) { free(L); free(P); return NULL; }
    if (tk_ldinit(L, "defs/vhdl_tok.def") != 0) {
        free(L); free(P); return NULL;
    }

    slen = (uint32_t)strlen(src);
    vh_lex(L, src, slen);

    vh_pinit(P, L);
    vh_parse(P);

    cv = (ce_val_t *)calloc(P->n_node, sizeof(ce_val_t));
    wv = (wi_val_t *)calloc(P->n_node, sizeof(wi_val_t));
    if (!cv || !wv) {
        free(cv); free(wv);
        tk_pfree(P); free(P);
        tk_ldfree(L); free(L);
        return NULL;
    }

    ce_eval(P, cv, P->n_node);
    wi_eval(P, cv, P->n_node, wv, P->n_node);

    M = lw_build(P, cv, wv, P->n_node);

    free(wv);
    free(cv);
    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    return M;
}

/* ---- VHDL Lexer Tests ---- */

static void vh_kwds(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);
    CHECK(L->n_kwd >= 100);  /* should have 118 keywords */
    CHECK(L->n_op >= 30);    /* should have 34 operators */
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_kwds)

static void vh_toks(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);

    int n = vh_lex_str("entity foo is end entity foo;", L);
    CHECK(n > 0);
    CHECK(L->n_err == 0);
    /* entity, foo, is, end, entity, foo, ; = 7 tokens */
    CHECK(L->n_tok >= 7);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_toks)

/* Case insensitive: SIGNAL = signal = Signal */
static void vh_case(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);

    vh_lex_str("SIGNAL Entity PROCESS", L);
    CHECK(L->n_err == 0);
    /* All three should be keywords despite mixed case */
    CHECK(L->tokens[0].type == TK_TOK_KWD);
    CHECK(L->tokens[1].type == TK_TOK_KWD);
    CHECK(L->tokens[2].type == TK_TOK_KWD);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_case)

/* VHDL comments: -- to end of line */
static void vh_cmnt(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);

    vh_lex_str("signal a -- this is a comment\nsignal b", L);
    CHECK(L->n_err == 0);
    /* signal, a, signal, b = 4 tokens (comment skipped) */
    CHECK(L->n_tok == 4);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_cmnt)

/* Bit string literal: X"FF" */
static void vh_bstr(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    CHECK(L != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);

    vh_lex_str("X\"FF\" B\"1010\"", L);
    CHECK(L->n_err == 0);
    CHECK(L->tokens[0].type == TK_TOK_INT_LIT);
    CHECK(L->tokens[1].type == TK_TOK_INT_LIT);

    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_bstr)

/* ---- VHDL Parser Tests ---- */

/* Parse entity with ports */
static void vh_enty(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    CHECK(L != NULL && P != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);

    vh_lex_str(
        "entity adder is port ("
        "  a : in std_logic_vector(7 downto 0);"
        "  b : in std_logic_vector(7 downto 0);"
        "  s : out std_logic_vector(7 downto 0)"
        "); end entity adder;"
        "architecture rtl of adder is begin "
        "  s <= a; "
        "end architecture rtl;",
        L);

    vh_pinit(P, L);
    vh_parse(P);

    CHECK(P->n_err == 0);
    /* Should have MODULE node with PORT children */
    CHECK(P->n_node > 5);

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_enty)

/* Parse process with case statement */
static void vh_proc(void)
{
    rt_mod_t *M = vh_rtl(
        "entity alu is port ("
        "  a : in std_logic_vector(7 downto 0);"
        "  op : in std_logic_vector(1 downto 0);"
        "  r : out std_logic_vector(7 downto 0)"
        "); end entity alu;"
        "architecture rtl of alu is begin "
        "  process(a, op) begin "
        "    case op is "
        "      when \"00\" => r <= a; "
        "      when \"01\" => r <= not a; "
        "      when others => r <= a; "
        "    end case; "
        "  end process; "
        "end architecture rtl;");

    CHECK(M != NULL);
    /* Should have MUX cells from case statement */
    CHECK(M->n_cell > 1);

    rt_free(M); free(M);
    PASS();
}
TH_REG("vhdl", vh_proc)

/* Module name comes through from entity */
static void vh_mnam(void)
{
    rt_mod_t *M = vh_rtl(
        "entity my_chip is port ("
        "  a : in std_logic"
        "); end entity my_chip;"
        "architecture rtl of my_chip is begin "
        "end architecture rtl;");

    CHECK(M != NULL);
    CHSTR(M->mod_name, "my_chip");

    rt_free(M); free(M);
    PASS();
}
TH_REG("vhdl", vh_mnam)
