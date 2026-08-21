/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * takahe.h -- open-source SystemVerilog synthesis
 *
 * Named after the takahe (Porphyrio hochstetteri), declared extinct in 1898
 * and found alive in the Murchison Mountains fifty years later. Like chip
 * design access, it was thought to be gone forever.
 *
 * The IEEE 1800 spec lives in .def files and the engine interprets them, so
 * a language change is a text file rather than a recompile.
 *
 * All types, limits and forward declarations live here.
 */

#ifndef TAKAHE_H
#define TAKAHE_H

/* Longest path we will build when hunting for a shipped .def or .txt. */
#define TK_PATH_MAX 1024

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kauri: arena allocator, bounds checking, string builder.
 * The memory safety foundation. No malloc in the hot path. */
#include "kauri.h"

/* ---- Version ----
 * Three numbers, and a string built from them rather than typed out beside
 * them. The banner and the Yosys writer used to carry their own copies, which
 * is two chances to disagree and no way to notice; bumping the patch left the
 * generated netlists claiming the old one. The install rules read these too,
 * so packaging cannot drift from what the binary reports. */
#define TK_VER_MAJOR 0
#define TK_VER_MINOR 1
#define TK_VER_PATCH 1

#define TK_STR_(x)   #x
#define TK_STR(x)    TK_STR_(x)
#define TK_VER_STRING \
    TK_STR(TK_VER_MAJOR) "." TK_STR(TK_VER_MINOR) "." TK_STR(TK_VER_PATCH)

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
    uint8_t       tok_ovf;   /* token pool filled, source truncated */
    uint8_t       str_ovf;   /* string pool filled, names came back empty */
} tk_lex_t;

/* ---- Error Entry ---- */

typedef struct {
    uint32_t  line;
    uint16_t  col;
    char      msg[128];
} tk_err_t;

/* ---- JEDEC fuse map (JESD3-C) ----
 * A 22V10 needs 5892 fuses. Larger CPLDs run to hundreds of thousands and
 * are refused rather than silently truncated. */

#define JD_MAX_FUSE    32768
#define JD_MAX_ERRS    32
#define JD_MAX_FIELD   (1u << 22)   /* guard on any single field */

typedef struct {
    uint32_t  qf;                      /* fuse count, QF */
    uint32_t  qp;                      /* pin count, QP */
    uint32_t  qv;                      /* test vector count, QV */

    uint8_t   fuse[JD_MAX_FUSE / 8];   /* fuse n is bit n&7 of byte n>>3 */
    uint8_t   seen[JD_MAX_FUSE / 8];   /* set by an L field, so F can fill */

    uint8_t   dflt;                    /* F, state of unlisted fuses */
    int8_t    secur;                   /* G, -1 when the field is absent */

    uint16_t  csum,  ccalc;            /* fuse checksum, given and computed */
    uint16_t  tsum,  tcalc;            /* transmission checksum, ditto */
    uint8_t   has_c, has_t;

    char      devid[64];               /* first N or J field, when present */

    tk_err_t  errors[JD_MAX_ERRS];
    uint32_t  n_err;
} jd_file_t;

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
    uint16_t kw_package;   uint16_t endpackage;
    uint16_t kw_import;
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
    struct {
        uint32_t off;          /* name string offset */
        uint16_t len;          /* name string length */
        uint16_t width;        /* bit width (0 = unknown) */
        uint32_t enum_noff[16]; /* enum value name offsets */
        uint16_t enum_nlen[16]; /* enum value name lengths */
        int32_t  enum_vals[16]; /* enum constant values */
        uint8_t  n_enum;       /* number of enum values */
    } tnames[TK_MAX_TNAMES];
    uint32_t          n_tname;

    /* Errors */
    tk_err_t          errors[TK_MAX_ERRORS];
    uint32_t          n_err;
    uint8_t           node_ovf;  /* AST pool filled, tree is incomplete */
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
int  ab_lex   (tk_lex_t *L, const char *src, uint32_t len);
void tk_ldfree(tk_lex_t *L);

/* VHDL parser: init + parse (reuses tk_parse_t and AST) */
int  vh_pinit (tk_parse_t *P, const tk_lex_t *L);
int  vh_parse (tk_parse_t *P);

/* ABEL parser: reuses tk_parse_t and AST */
int  ab_parse (tk_parse_t *P, const tk_lex_t *L);

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
    RT_LUT,            /* Arbitrary function, truth table in
                        * the cd library at rt_cell_t.cdix.
                        * How a31o gets to be a first-class
                        * citizen instead of an also-ran.    */
    RT_DFFS,           /* DFF with async SET: comes up at 1.
                        * ins[2] is SET_B, active low, same
                        * shape as RT_DFFR but the opposite
                        * opinion about where it starts.     */
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
    /* Source location of the AST node that produced this net.
     * Zero means "no provenance" (synthesised by a later pass
     * or simply not populated yet). Self-contained on purpose:
     * the AST is freed long before opt/xform/tech run. */
    uint32_t  line;
    uint16_t  col;
    uint16_t  pad;
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
    /* Source location, same contract as rt_net_t. Zero means
     * the cell was synthesised by a pass that did not preserve
     * provenance, which is fine for now and will be tightened
     * pass by pass over time. */
    uint32_t   line;
    uint16_t   col;
    /* RT_LUT only: index of this cell's truth table in the cd
     * library. Sits in what used to be padding, so the struct
     * costs the same as it did yesterday. */
    uint16_t   cdix;
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

    /* First net index of the top-level module (for equiv) */
    uint32_t    top_net_lo;

    /* Inferred memories (arrays with read/write ports) */
    struct {
        uint32_t name_off;
        uint16_t name_len;
        uint32_t data_w;    /* bits per element */
        uint32_t depth;     /* number of elements */
        uint32_t addr_w;    /* ceil(log2(depth)) */
        /* Technology-mapping result. prim_set goes to 1 when
         * mp_mmap finds a primitive that fits this memory and
         * prim_idx points into the loaded ml_lib_t.prims[]. */
        uint8_t  prim_set;
        uint8_t  prim_idx;
        uint8_t  pad[2];
    } mems[RT_MAX_MEMS];
    uint32_t    n_mem;
} rt_mod_t;

/* ---- Memory primitive library ----
 * Loaded from defs/mems_<family>.def files. One file per
 * FPGA family or PDK. The matcher walks rt_mod_t.mems[]
 * and picks the lowest-cost primitive that fits, falling
 * back to "leave as RT_MEMRD/RT_MEMWR" when nothing fits. */

#define ML_MAX_PRIMS    16
#define ML_MAX_PORTS    4
#define ML_MAX_WIDTHS   8
#define ML_NAME_LEN     40

typedef enum {
    ML_TYPE_BRAM = 0,    /* FPGA block RAM, sync, configurable shape */
    ML_TYPE_SRAM,        /* ASIC hard macro, fixed shape              */
    ML_TYPE_LUTRAM,      /* FPGA distributed RAM, async read          */
    ML_TYPE_SPRAM,       /* iCE40-style single-port RAM               */
    ML_TYPE_COUNT
} ml_ptype_t;

typedef enum {
    ML_PORT_AR = 0,      /* async read only      */
    ML_PORT_SR,          /* sync read only       */
    ML_PORT_AW,          /* async write only     */
    ML_PORT_SW,          /* sync write only      */
    ML_PORT_ARSW,        /* async read + sync write (LUTRAM shape) */
    ML_PORT_SRSW,        /* sync read + sync write (1RW SRAM port) */
    ML_PORT_COUNT
} ml_pkind_t;

typedef struct {
    uint8_t  kind;       /* ml_pkind_t */
    char     name[8];    /* "R" "W" "RW" "A" "B"            */
    uint8_t  clk;        /* 0=none 1=posedge 2=negedge      */
    uint8_t  has_we;
    uint8_t  has_re;
    uint8_t  has_clken;
    uint8_t  be_gran;    /* 0=no BE, N=BE granularity bits  */
    uint8_t  active_low; /* csb/web active low (SKY130)     */
    uint8_t  abswap;     /* swap N LSBs of address (iCE40)  */
    uint8_t  pad;
} ml_port_t;

typedef struct {
    char       name[ML_NAME_LEN];
    uint8_t    type;             /* ml_ptype_t                       */
    uint8_t    abits;            /* address bits at widest config    */
    uint8_t    n_widths;
    uint8_t    widths[ML_MAX_WIDTHS];
    uint32_t   size;             /* total bits                       */
    uint8_t    init;             /* 0=none 1=zero 2=any              */
    uint8_t    pad;
    uint16_t   cost;             /* matcher cost, lower is preferred */
    uint8_t    n_ports;
    ml_port_t  ports[ML_MAX_PORTS];
} ml_prim_t;

typedef struct {
    ml_prim_t  prims[ML_MAX_PRIMS];
    uint8_t    n_prim;
} ml_lib_t;

/* Load a memory primitive library from a .def file. Returns 0
 * on success, non-zero on parse failure. The library can be
 * loaded incrementally: call once per .def file and the rules
 * accumulate up to ML_MAX_PRIMS. */
int          ml_load (ml_lib_t *lib, const char *path);

/* Walk M->mems[] and attempt to map each to a primitive in
 * the library. Sets prim_set + prim_idx when a match is
 * found, leaves them at zero otherwise. Returns the number
 * of memories successfully mapped. */
int          mp_mmap (rt_mod_t *M, const ml_lib_t *lib);

/* RTL IR */
int          rt_init (rt_mod_t *M, uint32_t max_net, uint32_t max_cell);
void         rt_free (rt_mod_t *M);
uint32_t     rt_anet (rt_mod_t *M, const char *name, uint16_t nlen,
                      uint32_t width, uint8_t port, uint8_t radix);
uint32_t     rt_acell(rt_mod_t *M, rt_ctype_t type, uint32_t out,
                      const uint32_t *ins, uint8_t n_in, uint32_t width);
/* Source-tagged variants. Same semantics, plus the cell or
 * net is marked with the AST line/col it came from. The
 * untagged forms above are thin wrappers that pass zero. */
uint32_t     rt_anet_at (rt_mod_t *M, const char *name, uint16_t nlen,
                         uint32_t width, uint8_t port, uint8_t radix,
                         uint32_t line, uint16_t col);
uint32_t     rt_acell_at(rt_mod_t *M, rt_ctype_t type, uint32_t out,
                         const uint32_t *ins, uint8_t n_in, uint32_t width,
                         uint32_t line, uint16_t col);

/* Returns non-zero if any pool overflowed since the most
 * recent rt_init. Used by main to decide whether to fire a
 * tk_abend dump at the end of a synthesis run. */
int          rt_ovflow(void);
void         rt_dump (const rt_mod_t *M);
uint32_t     rt_undrv(const rt_mod_t *M);
const char  *rt_cname(rt_ctype_t t);

/* RTL lowering */
int        lw_lower(const tk_parse_t *P, const ce_val_t *cv,
                    const wi_val_t *wv, uint32_t nvals);
rt_mod_t  *lw_build(const tk_parse_t *P, const ce_val_t *cv,
                    const wi_val_t *wv, uint32_t nvals);
rt_mod_t  *lw_build_r(const tk_parse_t *P, const ce_val_t *cv,
                      const wi_val_t *wv, uint32_t nvals,
                      uint8_t radix);
/* lw_build_n (structural netlist mode) is declared further down,
 * once the Liberty and cell-def types exist. */

/* Optimisation — see below for op_cprop/op_opt (need cd_lib_t) */
uint32_t    *rt_fan  (rt_mod_t *M);
int          op_dce  (rt_mod_t *M);

/* Technology mapping / export */
int          mp_blif (const rt_mod_t *M, FILE *fp);
int          mp_yosys(const rt_mod_t *M, FILE *fp);
uint64_t     mp_hash (const rt_mod_t *M);

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
    /* Asynchronous set. A cell with `preset` in its ff group comes up at
     * one, not zero, and calling that "a flop with no reset" quietly
     * corrupts whatever it feeds from the very first cycle. */
    uint8_t   set_pin;   /* index for set (0xFF=none)    */
    /* Isolation, level shifter, always-on, integrated clock
     * gate. These compute ordinary functions and are emphatically
     * not ordinary gates, so they stay out of the binding table.
     * An isolation cell is an OR gate the way a fire door is a
     * door. */
    uint8_t   special;
    /* clocked_on starts with '!', so this one samples on the
     * falling edge. RT_DFF means posedge, and binding it to a
     * negedge cell inverts the whole design very quietly. */
    uint8_t   negclk;

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
    char      name[64];   /* from the library() header, for reports */
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

/* Liberty function string into a cd truth table (tk_lfn.c).
 * Returns -1 for anything not expressible as one, sequential
 * cells included, rather than guessing. */
int  lb_fbld(const lb_lib_t *lib, const lb_cell_t *cell,
             cd_cell_t *out);
const lb_cell_t *lb_fcel(const lb_lib_t *lib, const char *name,
                         uint16_t len);
int  lb_cdix(const lb_lib_t *lib, const lb_cell_t *cell,
             cd_lib_t *cd);

/* Structural netlist mode: unresolved instances are looked up
 * in the Liberty library rather than treated as black boxes. */
rt_mod_t  *lw_build_n(const tk_parse_t *P, const ce_val_t *cv,
                      const wi_val_t *wv, uint32_t nvals,
                      const lb_lib_t *lib, cd_lib_t *cdl);

/* ---- Sequential structure recovery (tk_seqr.c) ----
 * What the flops are doing, worked out from the netlist graph
 * rather than from a name someone helpfully left behind. */

#define SQ_MAX_FF   1024   /* flops we'll analyse           */
#define SQ_MAX_SRC     4   /* distinct flop sources stored  */
#define SQ_MAX_STK  4096   /* cone-walk worklist depth      */

typedef enum {
    SQ_HOLD = 0,   /* D depends on no flop but possibly itself */
    SQ_SHIFT,      /* D depends on exactly one other flop      */
    SQ_FSM         /* D depends on several: real state         */
} sq_kind_t;

typedef struct {
    uint32_t cell;              /* flop cell index            */
    uint32_t q, d;              /* output and D-input nets    */
    uint32_t clk, rst;          /* control nets               */
    uint32_t src[SQ_MAX_SRC];   /* other flops feeding D      */
    uint32_t nsrc;
    uint32_t indeg, outdeg;     /* degrees in the shift graph */
    uint8_t  self;              /* D cone includes own Q      */
    uint8_t  kind;
    int32_t  chain;             /* chain id, -1 if none       */
    uint32_t pos;               /* position along that chain  */
    int32_t  grp;               /* control-signal group id    */
} sq_ff_t;

typedef struct {
    sq_ff_t  ff[SQ_MAX_FF];
    uint32_t n_ff;
    uint32_t n_chain;
    uint32_t chlen[SQ_MAX_FF];
    uint32_t chhead[SQ_MAX_FF];
    /* Registers grouped by shared control signals, after DANA.
     * Flops that clock and reset together are usually one word. */
    uint32_t n_grp;
    uint32_t grplen[SQ_MAX_FF];
    uint32_t grpff[SQ_MAX_FF];  /* a representative flop per group */
} sq_res_t;

int  sq_scan(const rt_mod_t *M, sq_res_t *R);
void sq_rep (const rt_mod_t *M, const sq_res_t *R);

/* ---- CNF construction (tk_cnf.c) ----
 * Tseitin encoding of a recovered netlist, optionally unrolled over time,
 * so a question about the circuit becomes a question for a SAT solver.
 * Literals follow the DIMACS convention: +v is v, -v is NOT v, and 0 is
 * not a variable. */

typedef struct {
    int32_t  *lits;      /* literal pool, clauses laid end to end */
    uint32_t *cls;       /* start offset of each clause           */
    uint32_t  n_lit, max_lit;
    uint32_t  n_cls, max_cls;
    uint32_t  n_var;
} cn_t;

int      cn_init(cn_t *C, uint32_t max_cls, uint32_t max_lit);
void     cn_free(cn_t *C);
uint32_t cn_var (cn_t *C);
int      cn_add (cn_t *C, const int32_t *lits, uint32_t n);
int      cn_unit(cn_t *C, int32_t lit);
/* Encode one cell's truth table: ins[] are variables, out is a variable. */
int      cn_lut (cn_t *C, const cd_cell_t *t, uint8_t outsel,
                 const uint32_t *ins, uint8_t n_in, uint32_t out);
int      cn_dmcs(const cn_t *C, FILE *fp);
/* Truth-table mask for a built-in gate type, and the clause generator that
 * turns any such mask into CNF. Shared with the fault injector so there is
 * one encoder and one place to be wrong. */
int      cn_gmsk(rt_ctype_t t, uint8_t n_in, uint32_t *mask);
int      cn_gate(cn_t *C, uint32_t mask, const uint32_t *ins, uint8_t n_in,
                 uint32_t out);

/* Encode a combinational bit-level module. vars[] must have room for
 * M->n_net entries and comes back mapping net -> variable. Returns -1 if
 * the module holds a cell it cannot encode (a wide operator that was
 * never bit-blasted, a latch, a memory), because a partial encoding
 * would prove the wrong thing. */
int      cn_mod (cn_t *C, const rt_mod_t *M, const cd_lib_t *cd,
                 uint32_t *vars);
/* Miter of two modules: same inputs, assert some output differs.
 * UNSAT means equivalent. Returns the variable asserted, or 0. */
uint32_t cn_mitr(cn_t *C, const rt_mod_t *A, const rt_mod_t *B,
                 const cd_lib_t *cd);

/* ---- SAT (tk_sat.c) ----
 * CDCL with watched literals, VSIDS and Luby restarts. Returns 1 for
 * satisfiable and fills model[1..n_var] with 0/1, 0 for unsatisfiable,
 * and -1 if it gave up on the conflict budget. */
int      sa_solve(const cn_t *C, uint8_t *model, uint64_t max_conf);

/* Unroll M over k cycles. Every input port named in inets[] gets a fresh
 * variable each cycle, recorded in inv[t * n_in + j]; every other input
 * port is held at zero, so pin the ones you care about with unit clauses.
 * Returns the variable carrying `net` at the last cycle. Flops start at
 * zero, which is where reset leaves them. */
uint32_t cn_unrl(cn_t *C, const rt_mod_t *M, const cd_lib_t *cd,
                 uint32_t k, const uint32_t *inets, uint32_t n_in,
                 uint32_t net, uint32_t *inv,
                 const uint32_t *wnets, uint32_t n_w, uint32_t *wv);

/* ---- Cycle simulation of a recovered netlist (tk_sim.c) ----
 * Two-valued, zero-delay, one posedge clock domain. sm_eval
 * returns 1 if the logic won't settle, which means a loop. */

typedef struct {
    uint8_t *val;      /* current value per net */
    uint32_t n_net;
} sm_st_t;

int      sm_init(sm_st_t *S, const rt_mod_t *M);
void     sm_free(sm_st_t *S);
uint32_t sm_net (const rt_mod_t *M, const char *name);
int      sm_set (sm_st_t *S, uint32_t net, uint8_t v);
int      sm_get (const sm_st_t *S, uint32_t net);
int      sm_eval(const rt_mod_t *M, const cd_lib_t *cd, sm_st_t *S);
int      sm_tick(const rt_mod_t *M, const cd_lib_t *cd, sm_st_t *S);

/* ---- Fault injection (tk_fi.c) ----
 * Asks whether a single upset can change anything observable. The design is
 * cut at the flops, so every flop output becomes a free input and every flop
 * D joins the real outputs as something worth watching. Two copies of the
 * combinational logic run against shared inputs, one of them with an XOR
 * spliced into a chosen node, and SAT is asked whether any input tells the
 * copies apart. Unsatisfiable is a proof that the upset is masked.
 *
 * Cutting at the flops hands the solver an arbitrary starting state, so a
 * reported fault may need a state the design cannot actually reach. That
 * errs the safe way, since a clean run still means clean.
 *
 * Wide operators are bit-blasted first, on a private copy, so the design
 * the caller goes on to emit is the one it asked for. Site net indices
 * therefore refer to that copy and mean nothing outside, which is why each
 * site carries its own name.
 *
 * fi_res_t runs to a few hundred KB, so it wants the heap, not a frame. */

#define FI_MAXFLT 4096   /* fault sites considered in one run */
#define FI_NAMEL  48     /* net name kept per site, truncated to fit */

typedef struct {
    uint32_t net;    /* net index within the working copy              */
    uint32_t sel;    /* CNF variable that switches it on               */
    uint8_t  seq;    /* flop output, so a stored upset not a glitch    */
    char     name[FI_NAMEL];
} fi_site_t;

typedef struct {
    fi_site_t site[FI_MAXFLT];
    uint32_t  n_site;
    uint32_t  bad[FI_MAXFLT];   /* indices into site[] that reached a watcher */
    uint32_t  n_bad;
    uint32_t  n_obs;            /* watched nets compared                      */
    uint32_t  n_rep;            /* replica flops assumed to agree             */
    uint8_t   trunc;            /* more sites than fit, so results are partial */
    uint8_t   gaveup;           /* solver hit its conflict budget             */
} fi_res_t;

/* comb 0 covers flop outputs alone, which is the stored-upset question.
 * comb 1 adds every gate output, which also catches glitches but reports
 * far more, since nothing masks a glitch on the last gate before a flop.
 * Returns the number of unprotected sites, or -1 if it could not encode. */
int  fi_chk(const rt_mod_t *M, const cd_lib_t *cd, int comb, fi_res_t *R);
void fi_rep(const fi_res_t *R);

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

/* TMR radiation hardening */
int  tm_tmr(rt_mod_t *M, int full);

/* Equivalence checking — combinational simulation */
int  eq_check(const rt_mod_t *A, const rt_mod_t *B);

/* FPGA mapping — emit nextpnr JSON for iCE40 */
int  fp_json(const rt_mod_t *M, FILE *fp);
int  es_cone(const rt_mod_t *M, uint32_t out_net,
             uint32_t *onm, int *non,
             uint32_t *offm, int *noff,
             uint32_t *inputs, int *nin);

/* Timing-driven optimisation: cell resizing on critical paths */
int          op_tdopt(rt_mod_t *M, const lb_lib_t *lib,
                      mp_bind_t *tbl, tk_fs_t clk_fs);

/* Gate-level Verilog emitter */
/* Emit recovered cells too: cd supplies the original library
 * gate name behind each RT_LUT. Gates in, HDL back out. */
int          em_vlogn(const rt_mod_t *M, const lb_lib_t *lib,
                      const mp_bind_t *tbl, const cd_lib_t *cd,
                      FILE *fp);
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

/* Locate a shipped data file such as "defs/sv_tok.def". Checks $TAKAHE_HOME,
 * the working directory, then beside the binary, so an installed or unpacked
 * takahe finds the definitions it cannot start without. */
const char *tk_data(const char *rel);
void        tk_data_init(const char *argv0);
void  tk_emsg (int eid, ...);
void  tk_abend(const char *mod, const char *reason,
               const rt_mod_t *M);

/* JEDEC fuse maps. Returns 0 when the file parsed and both checksums agreed,
 * -1 otherwise, with the reasons in J->errors and the fuses still populated. */
int jd_read(jd_file_t *J, const char *path);
int jd_getf(const jd_file_t *J, uint32_t n);

/* Utility */
const char *tk_tokstr(tk_toktype_t t);
const char *tk_kwstr (const tk_lex_t *L, uint16_t id);
const char *tk_opstr (const tk_lex_t *L, uint16_t id);
const char *tk_aststr(tk_ast_type_t t);

#endif /* TAKAHE_H */
