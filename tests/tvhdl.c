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

/* ---- A clocked process must actually be clocked ----
 * VHDL puts the edge inside the body, not the sensitivity list, so
 * the clock has to be recovered from rising_edge(). This used to write
 * a placeholder that the lowerer compared against "posedge", never
 * matched, and every VHDL flop came out with its clock tied to net 0.
 * Silently. For months. */

static void vh_clk(void)
{
    rt_mod_t *M = vh_rtl(
        "library ieee; use ieee.std_logic_1164.all;"
        "entity ff is port ("
        "  clk : in std_logic; rst_n : in std_logic;"
        "  d : in std_logic; q : out std_logic"
        "); end entity ff;"
        "architecture rtl of ff is signal r : std_logic; begin "
        "process(clk, rst_n) begin "
        "  if rst_n = '0' then r <= '0';"
        "  elsif rising_edge(clk) then r <= d;"
        "  end if; end process; q <= r;"
        "end architecture rtl;");
    uint32_t i, nclk = 0, nrst = 0, found = 0;

    CHECK(M != NULL);

    for (i = 1; i < M->n_net; i++) {
        const char *s = M->strs + M->nets[i].name_off;
        if (M->nets[i].name_len == 3 && memcmp(s, "clk", 3) == 0) nclk = i;
        if (M->nets[i].name_len == 5 && memcmp(s, "rst_n", 5) == 0) nrst = i;
    }
    CHECK(nclk != 0);
    CHECK(nrst != 0);

    for (i = 1; i < M->n_cell; i++) {
        const rt_cell_t *c = &M->cells[i];
        if (c->type != RT_DFF && c->type != RT_DFFR) continue;
        found++;
        /* the whole point: a real clock net, not zero */
        CHECK(c->n_in >= 2);
        CHECK(c->ins[1] == nclk);
        /* and the async reset survived the trip */
        CHECK(c->type == RT_DFFR);
        CHECK(c->ins[2] == nrst);
    }
    CHECK(found > 0);

    rt_free(M); free(M);
    PASS();
}
TH_REG("vhdl", vh_clk)

/* ---- A falling-edge clock must be refused, not mis-synthesised ----
 * RT_DFF is posedge by definition and the lowerer reads negedge as
 * "async reset", so emitting one anyway would produce a flop with no
 * clock and a reset that isn't one. Better to say so. */

static void vh_fall(void)
{
    tk_lex_t *L = (tk_lex_t *)calloc(1, sizeof(tk_lex_t));
    tk_parse_t *P = (tk_parse_t *)calloc(1, sizeof(tk_parse_t));
    static const char *src =
        "library ieee; use ieee.std_logic_1164.all;"
        "entity ff is port ("
        "  clk : in std_logic; d : in std_logic; q : out std_logic"
        "); end entity ff;"
        "architecture rtl of ff is signal r : std_logic; begin "
        "process(clk) begin "
        "  if falling_edge(clk) then r <= d; end if;"
        "  end process; q <= r;"
        "end architecture rtl;";

    CHECK(L != NULL && P != NULL);
    CHECK(tk_ldinit(L, "defs/vhdl_tok.def") == 0);
    vh_lex(L, src, (uint32_t)strlen(src));
    vh_pinit(P, L);
    vh_parse(P);

    CHECK(P->n_err > 0);          /* refused, rather than quietly wrong */

    tk_pfree(P); free(P);
    tk_ldfree(L); free(L);
    PASS();
}
TH_REG("vhdl", vh_fall)
