/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * takahe.h -- Open-Source SystemVerilog Synthesis Tool
 *
 * Named after the takahe (Porphyrio hochstetteri): declared
 * extinct in 1898, rediscovered alive in the Murchison Mountains
 * in 1948, and slowly brought back through conservation.
 * Like chip design access, it was thought to be gone forever.
 *
 * Definition-driven: the IEEE 1800 spec lives in .def files,
 * the engine just interprets them. When the spec changes, you
 * edit a text file, not C code. When you want ternary cells,
 * you write a .def file.
 *
 * All types, limits, forward declarations live here.
 * One header to route them all, and in the silicon bind them.
 */

#ifndef TAKAHE_H
#define TAKAHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kauri: arena allocator, bounds checking, string builder.
 * The memory safety foundation. No malloc in the hot path. */
#include "kauri.h"

/* ---- Version ---- */
#define TK_VER_MAJOR 0
#define TK_VER_MINOR 1
#define TK_VER_PATCH 0

/* ---- Pool Limits ----
 * Fixed at compile time. Like foundry design rules:
 * if you exceed them, you need a bigger process node. */

#define TK_MAX_TOKENS   1048576   /* 1M tokens (large SoC designs)  */
#define TK_MAX_NODES    1048576   /* 1M AST nodes                   */
#define TK_MAX_STRS     (4u * 1024u * 1024u) /* 4MB string pool    */
#define TK_MAX_KWDS     512       /* keyword definitions from .def   */
#define TK_MAX_OPS      128       /* operator definitions from .def  */
#define TK_MAX_NETS     262144    /* nets in the IR                  */
#define TK_MAX_CELLS    262144    /* cell instances in the IR        */
#define TK_MAX_MODS     1024      /* module definitions              */
#define TK_MAX_PORTS    16384     /* ports across all modules        */
#define TK_MAX_PARAMS   8192      /* parameters across all modules   */
#define TK_MAX_ERRORS   256       /* error diagnostics               */
#define TK_MAX_TNAMES   512       /* typedef/enum/struct names       */

/* ---- Radix ----
 * Binary is just radix=2. The engine doesn't care.
 * Ternary, MVL, stochastic -- all just different numbers. */

#define TK_RADIX_BIN  2
#define TK_RADIX_TER  3

/* ---- Data Type ---- */

typedef struct {
    uint32_t  width;      /* number of digits                 */
    uint8_t   radix;      /* 2=binary, 3=ternary, N=arbitrary */
    uint8_t   is_signed;  /* 0=unsigned, 1=signed             */
    uint8_t   is_packed;  /* 0=unpacked, 1=packed             */
    uint8_t   pad;
} tk_dtype_t;

/* ---- Handle (Burroughs-style tagged descriptor) ----
 * Every IR object gets a handle with type tag + generation
 * counter. Use-after-free is structurally impossible:
 * stale generation = error, not corruption. */

typedef enum {
    TK_TAG_NONE = 0,
    TK_TAG_NET,
    TK_TAG_CELL,
    TK_TAG_PORT,
    TK_TAG_MOD,
    TK_TAG_PARAM
} tk_tag_t;

typedef struct {
    uint32_t  index;      /* pool index                       */
    uint16_t  gen;        /* generation counter                */
    uint8_t   tag;        /* tk_tag_t                          */
    uint8_t   pad;
} tk_hdl_t;

#define TK_HDL_NULL ((tk_hdl_t){0, 0, TK_TAG_NONE, 0})

/* ---- Token Types ---- */

typedef enum {
    TK_TOK_EOF = 0,
    TK_TOK_IDENT,        /* identifier                        */
    TK_TOK_KWD,          /* keyword (index into kwd table)    */
    TK_TOK_INT_LIT,      /* integer literal                   */
    TK_TOK_REAL_LIT,     /* real literal                      */
    TK_TOK_STR_LIT,      /* string literal                    */
    TK_TOK_OP,           /* operator (index into op table)    */
    TK_TOK_PREPROC,      /* preprocessor directive            */
    TK_TOK_SYSTASK,      /* system task/function              */
    TK_TOK_COMMENT,      /* comment (skipped normally)        */
    TK_TOK_ERROR         /* lexer error                       */
} tk_toktype_t;

/* ---- Token ---- */

typedef struct {
    tk_toktype_t type;
    uint16_t     sub;     /* keyword/op index into .def table  */
    uint32_t     off;     /* offset into string pool           */
    uint16_t     len;     /* length of text                    */
    uint32_t     line;    /* source line number                */
    uint16_t     col;     /* source column                     */
} tk_token_t;

/* ---- Keyword Definition (loaded from sv_tok.def) ---- */

typedef struct {
    uint32_t  name_off;   /* offset into string pool           */
    uint16_t  name_len;   /* length of keyword string          */
    uint16_t  id;         /* sequential ID                     */
} tk_kwdef_t;

/* ---- Operator Definition (loaded from sv_tok.def) ---- */

typedef struct {
    uint32_t  name_off;   /* offset into string pool (name)    */
    uint16_t  name_len;
    uint32_t  chars_off;  /* offset into string pool (chars)   */
    uint16_t  chars_len;
    uint16_t  id;         /* sequential ID                     */
} tk_opdef_t;

/* ---- Lexer Context ---- */

typedef struct {
    /* Source */
    const char   *src;
    uint32_t      src_len;
    uint32_t      pos;
    uint32_t      line;
    uint16_t      col;

    /* Output tokens */
    tk_token_t   *tokens;
    uint32_t      n_tok;
    uint32_t      max_tok;

    /* String pool */
    char         *strs;
    uint32_t      str_len;
    uint32_t      str_max;

    /* Definitions (loaded from sv_tok.def) */
    tk_kwdef_t    kwds[TK_MAX_KWDS];
    uint32_t      n_kwd;
    tk_opdef_t    ops[TK_MAX_OPS];
    uint32_t      n_op;

    /* Errors */
    uint32_t      n_err;
} tk_lex_t;

/* ---- Error Entry ---- */

typedef struct {
    uint32_t  line;
    uint16_t  col;
    char      msg[128];
} tk_err_t;

/* ---- Journal Entry (CICS-style transaction log) ----
 * Every mutation to the IR is journaled. If a pass fails,
 * discard the journal and revert. No half-optimised netlists. */

typedef enum {
    TK_JRN_ADD_NET = 0,
    TK_JRN_ADD_CELL,
    TK_JRN_DEL_NET,
    TK_JRN_DEL_CELL,
    TK_JRN_MOD_NET,
    TK_JRN_MOD_CELL,
    TK_JRN_CHECKPOINT
} tk_jrn_op_t;

typedef struct {
    tk_jrn_op_t  op;
    tk_hdl_t     hdl;
    uint32_t     data[4];    /* pass-specific payload */
} tk_jrn_t;

/* ---- AST Node Types ---- */

typedef enum {
    /* Top level */
    TK_AST_ROOT = 0,
    TK_AST_MODULE,        /* module declaration              */
    TK_AST_PORT,          /* port declaration                */
    TK_AST_PARAM,         /* parameter declaration           */
    TK_AST_LOCALPARAM,    /* localparam declaration          */

    /* Types */
    TK_AST_TYPEDEF,       /* typedef                         */
    TK_AST_ENUM_DEF,      /* enum definition                 */
    TK_AST_STRUCT_DEF,    /* struct/union definition          */
    TK_AST_MEMBER,        /* struct member                    */
    TK_AST_TYPE_SPEC,     /* type specifier (logic, wire...) */

    /* Declarations */
    TK_AST_NET_DECL,      /* wire/logic/reg declaration       */
    TK_AST_VAR_DECL,      /* variable declaration             */

    /* Assignments */
    TK_AST_ASSIGN,        /* continuous assignment (assign)   */
    TK_AST_BLOCK_ASSIGN,  /* blocking (=)                    */
    TK_AST_NONBLOCK,      /* non-blocking (<=)               */

    /* Always blocks */
    TK_AST_ALWAYS_COMB,   /* always_comb                     */
    TK_AST_ALWAYS_FF,     /* always_ff                       */
    TK_AST_ALWAYS_LATCH,  /* always_latch                    */
    TK_AST_ALWAYS,        /* plain always                    */
    TK_AST_SENS_LIST,     /* sensitivity list                */
    TK_AST_SENS_EDGE,     /* posedge/negedge                 */

    /* Control flow */
    TK_AST_IF,            /* if statement                    */
    TK_AST_CASE,          /* case/casex/casez                */
    TK_AST_CASE_ITEM,     /* case item                       */
    TK_AST_FOR,           /* for loop                        */
    TK_AST_WHILE,         /* while loop                      */
    TK_AST_BEGIN_END,     /* begin...end block               */

    /* Generate */
    TK_AST_GENERATE,      /* generate block                  */
    TK_AST_GENVAR,        /* genvar declaration               */
    TK_AST_GEN_FOR,       /* generate for                    */
    TK_AST_GEN_IF,        /* generate if                     */

    /* Expressions */
    TK_AST_IDENT,         /* identifier                      */
    TK_AST_INT_LIT,       /* integer literal                 */
    TK_AST_REAL_LIT,      /* real literal                    */
    TK_AST_STR_LIT,       /* string literal                  */
    TK_AST_BINARY_OP,     /* binary expression (a + b)       */
    TK_AST_UNARY_OP,      /* unary expression (!a, ~a)       */
    TK_AST_TERNARY,       /* ternary (a ? b : c)             */
    TK_AST_CONCAT,        /* concatenation {a, b}            */
    TK_AST_REPLICATE,     /* replication {N{expr}}           */
    TK_AST_INDEX,         /* bit/part select a[i]            */
    TK_AST_RANGE,         /* range [hi:lo]                   */
    TK_AST_MEMBER_ACC,    /* member access a.b               */
    TK_AST_CALL,          /* function/system call            */
    TK_AST_CAST,          /* type cast                       */

    /* Misc */
    TK_AST_INSTANCE,      /* module instantiation            */
    TK_AST_CONN,          /* port connection (.name(expr))   */

    TK_AST_COUNT
} tk_ast_type_t;

/* ---- AST Node ---- */

typedef struct {
    tk_ast_type_t type;
    uint32_t      first_child;  /* index of first child (0=none)   */
    uint32_t      last_child;   /* index of last child  (0=none)   */
    uint32_t      next_sib;     /* index of next sibling (0=none)  */
    uint16_t      op;           /* operator for expr nodes         */

    /* Payload: text reference or numeric value */
    union {
        struct { uint32_t off; uint16_t len; } text;
        int64_t  ival;
        double   fval;
    } d;

    uint32_t      line;
    uint16_t      col;
} tk_node_t;

/* ---- Pre-computed Keyword IDs ----
 * The parser checks ~25 keywords on every token. Doing
 * strcmp("endmodule") a million times is for people who
 * bill by the CPU cycle. We look them up once at init
 * and compare by uint16_t thereafter. */

#define TK_KW_NONE 0xFFFFu  /* keyword not found in .def */

typedef struct {
    uint16_t module;       uint16_t endmodule;
    uint16_t begin;        uint16_t end;
    uint16_t input;        uint16_t output;
    uint16_t inout;        uint16_t parameter;
    uint16_t localparam;   uint16_t assign;
    uint16_t always;       uint16_t always_comb;
    uint16_t always_ff;    uint16_t always_latch;
    uint16_t kw_if;        uint16_t kw_else;
    uint16_t kw_case;      uint16_t casex;
    uint16_t casez;        uint16_t endcase;
    uint16_t kw_for;       uint16_t generate;
    uint16_t endgenerate;  uint16_t genvar;
    uint16_t kw_default;   uint16_t posedge;
    uint16_t negedge;      uint16_t kw_or;
    uint16_t kw_typedef;   uint16_t kw_enum;
    uint16_t kw_struct;    uint16_t kw_union;
    uint16_t packed;       uint16_t signed_kw;
    uint16_t unsigned_kw;  uint16_t logic;
    uint16_t wire;         uint16_t reg;
    uint16_t bit;          uint16_t integer;
    uint16_t kw_int;       uint16_t byte_kw;
    uint16_t shortint;     uint16_t longint;
    uint16_t real;         uint16_t shortreal;
    uint16_t realtime;     uint16_t time_kw;
    uint16_t unique;       uint16_t priority;
    uint16_t initial;      uint16_t task;
    uint16_t endtask;      uint16_t kw_function;
    uint16_t endfunction;
    /* ---- VHDL keywords (TK_KW_NONE when in SV mode) ---- */
    uint16_t entity;       uint16_t architecture;
    uint16_t vh_of;        uint16_t vh_is;
    uint16_t vh_begin;     uint16_t vh_end;
    uint16_t vh_port;      uint16_t vh_generic;
    uint16_t vh_signal;    uint16_t vh_variable;
    uint16_t vh_constant;  uint16_t vh_process;
    uint16_t vh_if;        uint16_t vh_then;
    uint16_t vh_elsif;     uint16_t vh_else;
    uint16_t vh_case;      uint16_t vh_when;
    uint16_t vh_others;    uint16_t vh_in;
    uint16_t vh_out;       uint16_t vh_inout;
    uint16_t vh_buffer;    uint16_t vh_downto;
    uint16_t vh_to;        uint16_t vh_for;
    uint16_t vh_generate;  uint16_t vh_loop;
    uint16_t vh_while;     uint16_t vh_exit;
    uint16_t vh_next;      uint16_t vh_return;
    uint16_t vh_not;       uint16_t vh_and;
    uint16_t vh_or;        uint16_t vh_xor;
    uint16_t vh_nand;      uint16_t vh_nor;
    uint16_t vh_xnor;      uint16_t vh_library;
    uint16_t vh_use;       uint16_t vh_all;
    uint16_t vh_component; uint16_t vh_subtype;
    uint16_t vh_type;      uint16_t vh_record;
    uint16_t vh_array;     uint16_t vh_range;
    uint16_t vh_null;      uint16_t vh_open;
    uint16_t vh_map;       uint16_t vh_select;
    uint16_t vh_with;      uint16_t vh_after;
} tk_kwid_t;

/* ---- Parser Context ---- */

typedef struct {
    /* Input */
    const tk_token_t *tokens;
    uint32_t          n_tok;
    uint32_t          pos;       /* current token index             */

    /* Lexer (for string pool access) */
    const tk_lex_t   *lex;

    /* Pre-computed keyword IDs -- looked up once at init,
     * compared by integer thereafter. Life's too short
     * for strcmp in a parser inner loop. */
    tk_kwid_t         kw;

    /* Context: inside generate block, begin/end contains
     * module items (instantiations, always, etc.) not just
     * statements. Like a customs zone where the rules change
     * depending on which building you're in. */
    uint8_t           in_gen;

    /* Non-blocking disambiguation: when set, op_prec
     * refuses to recognise <= as a comparison operator.
     * This lets pk_stmt pick it up as non-blocking assign.
     * Set only for LHS parsing in statement context,
     * cleared inside parenthesised sub-expressions where
     * <= is genuinely a comparison. The SystemVerilog
     * ambiguity that launched a thousand workarounds. */
    uint8_t           no_le;

    /* Output AST */
    tk_node_t        *nodes;
    uint32_t          n_node;
    uint32_t          max_node;

    /* Type name registry (typedef/enum/struct names).
     * When the parser sees `typedef ... foo_t;` it registers
     * foo_t here. Later, when it sees `foo_t x;` it knows
     * foo_t is a type, not an identifier. Same problem as
     * BarraCUDA's tnames[] for C cast disambiguation. */
    struct { uint32_t off; uint16_t len; } tnames[TK_MAX_TNAMES];
    uint32_t          n_tname;

    /* Errors */
    tk_err_t          errors[TK_MAX_ERRORS];
    uint32_t          n_err;
} tk_parse_t;

/* ---- Public API ---- */

/* ---- Constant Expression Value ---- */

typedef struct {
    int64_t  val;
    uint32_t width;
    uint8_t  valid;
    uint8_t  is_signed;
} ce_val_t;

/* Preprocessor */
int  tk_preproc(const char *src, uint32_t src_len,
                char *out, uint32_t out_max,
                uint32_t *out_len,
                const char **defines, uint32_t n_defines);

/* Lexer: definition loading + tokenisation */
int  tk_ldinit(tk_lex_t *L, const char *def_path);
int  tk_lex   (tk_lex_t *L, const char *src, uint32_t len);
int  vh_lex   (tk_lex_t *L, const char *src, uint32_t len);
void tk_ldfree(tk_lex_t *L);

/* VHDL parser: init + parse (reuses tk_parse_t and AST) */
int  vh_pinit (tk_parse_t *P, const tk_lex_t *L);
int  vh_parse (tk_parse_t *P);

/* Constant expression evaluator */
int  ce_eval(const tk_parse_t *P, ce_val_t *vals, uint32_t nvals);

/* Elaboration */
int  el_elab(tk_parse_t *P, ce_val_t *cvals, uint32_t nvals);

/* Hierarchy flattening */
int  fl_flat(tk_parse_t *P);

/* Generate expansion */
int  ge_expand(tk_parse_t *P);

/* Width inference — defined here because the lowering pass
 * (lw_lower) needs it as a parameter type. */
typedef struct { uint32_t width; uint8_t resolved; } wi_val_t;
int  wi_eval(const tk_parse_t *P, const ce_val_t *cv,
             uint32_t nvals, wi_val_t *wv, uint32_t nwv);

/* Parser */
int  tk_pinit(tk_parse_t *P, const tk_lex_t *L);
int  tk_parse(tk_parse_t *P);
void tk_pfree(tk_parse_t *P);
void tk_pdump(const tk_parse_t *P, uint32_t node, int depth);

/* ---- RTL Cell Types ----
 * The periodic table of digital logic. Every operator
 * the designer writes becomes one of these. DFF is the
 * hydrogen: fundamental, everywhere, and occasionally
 * explosive when mishandled. */

typedef enum {
    RT_DFF = 0,        /* D flip-flop (posedge clk)         */
    RT_DFFR,           /* DFF with async reset               */
    RT_DLAT,           /* D latch (level-sensitive)          */
    RT_AND,            /* AND gate (N inputs)                */
    RT_OR,             /* OR gate                            */
    RT_XOR,            /* XOR gate                           */
    RT_NAND,           /* NAND gate                          */
    RT_NOR,            /* NOR gate                           */
    RT_XNOR,          /* XNOR gate                          */
    RT_NOT,            /* Inverter                           */
    RT_BUF,            /* Buffer (for driving)               */
    RT_MUX,            /* 2-to-1 multiplexer                 */
    RT_ADD,            /* Adder                              */
    RT_SUB,            /* Subtractor                         */
    RT_MUL,            /* Multiplier                         */
    RT_SHL,            /* Left shift                         */
    RT_SHR,            /* Right (logical) shift              */
    RT_SHRA,           /* Right (arithmetic) shift           */
    RT_EQ,             /* Equality compare                   */
    RT_NE,             /* Not-equal compare                  */
    RT_LT,             /* Less-than (signed or unsigned)     */
    RT_LE,             /* Less-or-equal                      */
    RT_GT,             /* Greater-than                       */
    RT_GE,             /* Greater-or-equal                   */
    RT_CONST,          /* Constant driver                    */
    RT_ASSIGN,         /* Direct assignment (wire)           */
    RT_CONCAT,         /* Concatenation                      */
    RT_SELECT,         /* Bit/part select                    */
    RT_PMUX,           /* Priority mux (case statement)      */
    RT_MEMRD,          /* Memory read port                   */
    RT_MEMWR,          /* Memory write port                  */
    RT_CELL_COUNT
} rt_ctype_t;

/* ---- RTL Net ---- */

#define RT_MAX_PIN  8  /* max pins per cell */

typedef struct {
    uint32_t  name_off;    /* string pool offset                */
    uint16_t  name_len;
    uint32_t  width;       /* bit width                         */
    uint8_t   radix;       /* 2=binary, 3=ternary               */
    uint8_t   is_port;     /* 0=internal, 1=input, 2=output, 3=inout */
    uint8_t   is_reg;      /* 1=registered (has DFF driver)     */
    uint16_t  gen;         /* generation counter                 */
    uint32_t  driver;      /* cell index that drives this net    */
} rt_net_t;

/* ---- RTL Cell ---- */

typedef struct {
    rt_ctype_t type;
    uint32_t   out;        /* output net index                   */
    uint32_t   ins[RT_MAX_PIN]; /* input net indices              */
    uint8_t    n_in;       /* number of inputs used              */
    uint32_t   width;      /* operation width                    */
    int64_t    param;      /* cell parameter (const value, etc.) */
    uint16_t   gen;        /* generation counter                 */
} rt_cell_t;

/* ---- RTL Module ---- */

#define RT_MAX_MEMS 64  /* memory blocks per module */

typedef struct rt_mod_s {
    rt_net_t   *nets;
    uint32_t    n_net;
    uint32_t    max_net;

    rt_cell_t  *cells;
    uint32_t    n_cell;
    uint32_t    max_cell;

    /* String pool (shared with lexer or separate) */
    char       *strs;
    uint32_t    str_len;
    uint32_t    str_max;

    /* Module name — set by lowerer from AST MODULE node */
    char        mod_name[64];

    /* Inferred memories (arrays with read/write ports) */
    struct {
        uint32_t name_off;
        uint16_t name_len;
        uint32_t data_w;    /* bits per element */
        uint32_t depth;     /* number of elements */
        uint32_t addr_w;    /* ceil(log2(depth)) */
    } mems[RT_MAX_MEMS];
    uint32_t    n_mem;
} rt_mod_t;

/* RTL IR */
int          rt_init (rt_mod_t *M, uint32_t max_net, uint32_t max_cell);
void         rt_free (rt_mod_t *M);
uint32_t     rt_anet (rt_mod_t *M, const char *name, uint16_t nlen,
                      uint32_t width, uint8_t port, uint8_t radix);
uint32_t     rt_acell(rt_mod_t *M, rt_ctype_t type, uint32_t out,
                      const uint32_t *ins, uint8_t n_in, uint32_t width);
void         rt_dump (const rt_mod_t *M);
const char  *rt_cname(rt_ctype_t t);

/* RTL lowering */
int        lw_lower(const tk_parse_t *P, const ce_val_t *cv,
                    const wi_val_t *wv, uint32_t nvals);
rt_mod_t  *lw_build(const tk_parse_t *P, const ce_val_t *cv,
                    const wi_val_t *wv, uint32_t nvals);
rt_mod_t  *lw_build_r(const tk_parse_t *P, const ce_val_t *cv,
                      const wi_val_t *wv, uint32_t nvals,
                      uint8_t radix);

/* Optimisation — see below for op_cprop/op_opt (need cd_lib_t) */
uint32_t    *rt_fan  (rt_mod_t *M);
int          op_dce  (rt_mod_t *M);

/* Technology mapping / export */
int          mp_blif (const rt_mod_t *M, FILE *fp);
int          mp_yosys(const rt_mod_t *M, FILE *fp);

/* ---- Exact Timing Arithmetic ----
 * Femtoseconds and attofarads. Integer. No floats.
 *
 * IEEE 754 double has 15 digits. Sounds like a lot until
 * you're adding up ten thousand gate delays at 3nm and
 * the accumulated rounding error is larger than your
 * timing margin. Then it sounds like a design rule
 * violation and a very expensive respin.
 *
 * IBM figured this out in 1964: if the number matters,
 * don't let the hardware round it. Packed decimal on
 * a System/360 doesn't lose a cent over a billion
 * transactions. Femtosecond integers on Takahe don't
 * lose a picosecond over a million gates.
 *
 * int64 in femtoseconds covers 0 to 9.2 million ns.
 * That's enough headroom to time-analyse a chip the
 * size of Wales running at the speed of a sedated
 * tortoise, and still have digits to spare. */

typedef int64_t  tk_fs_t;   /* femtoseconds — the SI unit of
                              * "your timing closure called,
                              *  it wants its precision back" */
typedef int64_t  tk_af_t;   /* attofarads — because someone
                              * at BIPM thought 10^-18 needed
                              * its own prefix and they were
                              * absolutely right              */
typedef int64_t  tk_uw_t;   /* microwatts — the unit of power
                              * that makes phone batteries weep */

/* Convert Liberty ns → femtoseconds. The +0.5 rounds the
 * float-to-int conversion correctly. After this point,
 * no more floats. The airlock is sealed. */
#define TK_NS2FS(ns)  ((tk_fs_t)((ns) * 1000000.0 + 0.5))
#define TK_PF2AF(pf)  ((tk_af_t)((pf) * 1000000.0 + 0.5))

/* ---- Liberty Library ----
 * Minimal subset of the Liberty format: cell names, pin names,
 * area, direction, function strings, and timing data.
 * Timing stored as femtosecond integers for exact arithmetic. */

#define LB_MAX_CELLS  512
#define LB_MAX_PINS   8
#define LB_MAX_STRS   (256u * 1024u)
#define LB_NLDM_SZ    7      /* NLDM table dimension (7×7 typical) */

/* NLDM delay table: 2D lookup indexed by input slew × output load.
 * All values in femtoseconds (delay/slew) or attofarads (load).
 * Interpolated by PCHIP for monotone cubic accuracy. */
typedef struct {
    tk_fs_t  idx1[LB_NLDM_SZ];  /* input slew axis (fs)     */
    tk_af_t  idx2[LB_NLDM_SZ];  /* output load axis (aF)    */
    tk_fs_t  vals[LB_NLDM_SZ * LB_NLDM_SZ]; /* delay grid (fs) */
    uint8_t  n1, n2;             /* actual dimensions        */
    uint8_t  valid;              /* 1 if table was populated */
} lb_nldm_t;

#define LB_DIR_IN   1
#define LB_DIR_OUT  2

typedef enum {
    LB_COMB = 0,   /* combinational                      */
    LB_DFF,        /* D flip-flop (ff group present)     */
    LB_DLAT,       /* D latch (latch group present)      */
    LB_TIE         /* tie-high / tie-low                 */
} lb_kind_t;

typedef struct {
    uint32_t  name_off;
    uint16_t  name_len;
    uint8_t   dir;        /* LB_DIR_IN / LB_DIR_OUT */
    uint8_t   is_clk;
    uint32_t  func_off;   /* function string (output pins) */
    uint16_t  func_len;

    /* Timing — integers only past this point.
     * Like a clean room: no floating contaminants. */
    tk_af_t   cap;        /* input capacitance (aF)         */
    lb_nldm_t rise;       /* cell_rise NLDM table           */
    lb_nldm_t fall;       /* cell_fall NLDM table           */
    lb_nldm_t tran_r;     /* rise_transition NLDM table     */
    lb_nldm_t tran_f;     /* fall_transition NLDM table     */
} lb_pin_t;

typedef struct {
    uint32_t  name_off;
    uint16_t  name_len;
    float     area;       /* um^2 — float is fine for area */
    lb_kind_t kind;
    lb_pin_t  pins[LB_MAX_PINS];
    uint8_t   n_pin;
    uint8_t   n_in;
    uint8_t   clk_pin;   /* index for clock (0xFF=none)  */
    uint8_t   d_pin;     /* index for D input             */
    uint8_t   q_pin;     /* index for Q output            */
    uint8_t   rst_pin;   /* index for reset (0xFF=none)  */

    /* Timing — worst-case corners. The pessimist's view
     * of a cell, which is the only safe view when you're
     * signing off a chip that costs $5M to respin. */
    tk_fs_t   delay_max;  /* max propagation delay (fs)   */
    tk_fs_t   setup;      /* setup time for DFF (fs)      */
    tk_fs_t   hold;       /* hold time for DFF (fs)       */
    tk_uw_t   leak_pw;    /* leakage power (µW) — the
                           * electricity bill of doing
                           * absolutely nothing            */
} lb_cell_t;

typedef struct {
    lb_cell_t cells[LB_MAX_CELLS];
    uint32_t  n_cell;
    char      strs[LB_MAX_STRS];
    uint32_t  str_len;
} lb_lib_t;

/* ---- Cell Definitions (from .def files) ----
 * The universal truth table. One structure describes a gate
 * for any radix, any logic family, any computing paradigm.
 * Binary AND, ternary min, stochastic multiply — all just
 * different truth tables in the same format.
 * The engine doesn't care about your number system. */

#define CD_MAX_CELLS  256     /* cell defs per library      */
#define CD_MAX_ROWS   256     /* truth table rows per cell   */
#define CD_MAX_PINS   8       /* pins per cell               */
#define CD_MAX_VALS   8       /* max values per truth row    */

typedef struct {
    int8_t   ins[CD_MAX_VALS];   /* input values              */
    int8_t   outs[CD_MAX_VALS];  /* output values             */
} cd_row_t;

typedef struct {
    char      name[32];      /* cell type name (AND, OR...)  */
    char      pins[CD_MAX_PINS][16]; /* pin names            */
    uint8_t   pdir[CD_MAX_PINS];    /* 1=in, 2=out           */
    uint8_t   n_pin;
    uint8_t   n_in;
    uint8_t   n_out;
    uint8_t   radix;         /* 2=binary, 3=ternary          */
    uint8_t   stoch;         /* 1=stochastic semantics       */
    cd_row_t  rows[CD_MAX_ROWS];
    uint16_t  n_row;
    char      func[64];      /* symbolic function string     */
} cd_cell_t;

typedef struct cd_lib_s {
    cd_cell_t cells[CD_MAX_CELLS];
    uint32_t  n_cell;
} cd_lib_t;

int  cd_load(cd_lib_t *lib, const char *path);
const cd_cell_t *cd_find(const cd_lib_t *lib, const char *name,
                          uint8_t radix);
int8_t cd_eval(const cd_cell_t *cell, const int8_t *ins,
               int8_t *outs);

/* Optimisation (cd may be NULL for pure binary) */
int          op_cprop(rt_mod_t *M, const cd_lib_t *cd);
int          op_pmatch(rt_mod_t *M);
int          op_opt  (rt_mod_t *M, const cd_lib_t *cd);

/* PCHIP interpolation — Fritsch & Carlson (1980) in int64.
 * For NLDM delay table lookup with exact arithmetic. */
int      pc_deriv(const int64_t *x, const int64_t *f,
                  int64_t *d, int n);
int64_t  pc_eval (int64_t x1, int64_t x2, int64_t f1, int64_t f2,
                  int64_t d1, int64_t d2, int64_t xe);
int64_t  pc_lkup (const int64_t *x, const int64_t *f,
                  int n, int64_t xe);
int64_t  pc_lk2d (const int64_t *x1, int n1,
                  const int64_t *x2, int n2,
                  const int64_t *f, int64_t xe1, int64_t xe2);

/* Liberty loader */
int          lb_load(lb_lib_t *lib, const char *path);

/* NLDM delay lookup via PCHIP: given slew + load, return delay */
tk_fs_t      lb_dly (const lb_nldm_t *tbl, tk_fs_t slew, tk_af_t load);

/* Cell binding: maps rt_ctype_t → library cell index */
typedef struct {
    uint32_t  cell_idx;
    uint8_t   valid;
} mp_bind_t;

int          mp_bind(const lb_lib_t *lib, mp_bind_t *tbl);

/* Static timing analysis */
int          ta_sta (const rt_mod_t *M, const lb_lib_t *lib,
                     const mp_bind_t *tbl, tk_fs_t clk_fs);

/* Bit-blast: decompose multi-bit cells to 1-bit */
int          mp_bblst(rt_mod_t *M);

/* Espresso two-level logic minimisation (Brayton et al., 1984) */
#define ES_MAXIN  16
#define ES_MAXCUB 256

int  es_mini(const uint32_t *onm, int non, const uint32_t *offm,
             int noff, int nin, void *cover);
int  op_espro(rt_mod_t *M);

/* Equivalence checking — combinational simulation */
int  eq_check(const rt_mod_t *A, const rt_mod_t *B);
int  es_cone(const rt_mod_t *M, uint32_t out_net,
             uint32_t *onm, int *non,
             uint32_t *offm, int *noff,
             uint32_t *inputs, int *nin);

/* Timing-driven optimisation: cell resizing on critical paths */
int          op_tdopt(rt_mod_t *M, const lb_lib_t *lib,
                      mp_bind_t *tbl, tk_fs_t clk_fs);

/* Gate-level Verilog emitter */
int          em_vlog(const rt_mod_t *M, const lb_lib_t *lib,
                     const mp_bind_t *tbl, FILE *fp);

/* CICS-style transaction journal (IBM, 1968).
 * Every mutation journaled. Failed passes roll back cleanly.
 * No half-optimised netlists. Ever. */
void      jr_begin (const rt_mod_t *M);
void      jr_acell (uint32_t ci);
void      jr_dcell (const rt_mod_t *M, uint32_t ci);
void      jr_mcell (const rt_mod_t *M, uint32_t ci);
void      jr_mbind (const mp_bind_t *tbl, uint8_t ct);
void      jr_commit(void);
void      jr_rback (rt_mod_t *M, mp_bind_t *tbl);
int       jr_active(void);
uint32_t  jr_count (void);

/* ABEND diagnostics and bilingual messages.
 * Kia ora, synthesis engine. */
void  tk_slang(int lang);      /* 0=en, 1=mi (Te Reo Māori) */
int   tk_glang(void);
void  tk_linit(const char *lang_dir);
void  tk_emsg (int eid, ...);
void  tk_abend(const char *mod, const char *reason,
               const rt_mod_t *M);

/* Utility */
const char *tk_tokstr(tk_toktype_t t);
const char *tk_kwstr (const tk_lex_t *L, uint16_t id);
const char *tk_opstr (const tk_lex_t *L, uint16_t id);
const char *tk_aststr(tk_ast_type_t t);

#endif /* TAKAHE_H */
