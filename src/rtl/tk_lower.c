/* Copyright (c) 2026 Zane Hambly
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * tk_lower.c -- AST to RTL lowering for Takahe
 *
 * The moment of truth: SystemVerilog becomes hardware.
 *
 *   always_ff @(posedge clk)  ->  D flip-flop
 *   always_comb               ->  combinational logic cone
 *   assign a = b & c          ->  AND gate
 *   if (sel) a = b else a = c ->  MUX
 *
 * This is where Takahe earns its keep. Everything before was
 * parsing and bookkeeping. This is synthesis.
 *
 * Like translating a recipe into a physical kitchen: the
 * words say "mix ingredients" but the hardware needs a
 * specific bowl, a specific whisk, and a specific order.
 *
 */

#include "takahe.h"
#include <inttypes.h>

/* ---- Lowering context ---- */


typedef struct {
    const tk_parse_t *P;
    const ce_val_t   *cv;
    const wi_val_t   *wv;
    uint32_t          nvals;
    rt_mod_t         *M;
    uint8_t           radix;  /* default radix for new nets */

    /* Net lookup: AST ident node -> RTL net index */
    uint32_t         *nmap;   /* indexed by AST node index */
    uint32_t          nmsz;
    uint32_t          net_lo; /* first net for current module scope */

    /* Sequential redirect: tracks current value of each net
     * within an always block. When x <= a; if(c) x <= b;
     * the second if's hold reads the first assignment's
     * output, not x itself. Bounded, stack-safe. */
    #define LW_RDIR_MAX 512
    struct { uint32_t net, cur, cell; } rdir[LW_RDIR_MAX];
    uint32_t n_rdir;

    /* Current write-enable condition for memory writes.
     * Set by lw_ifmux when descending into an if body,
     * cleared when leaving. 0 = unconditional. */
    uint32_t mem_we;
} lw_ctx_t;

/* ---- Sequential redirect helpers ---- */

static uint32_t
lw_rcur(const lw_ctx_t *C, uint32_t net)
{
    uint32_t i;
    for (i = 0; i < C->n_rdir; i++)
        if (C->rdir[i].net == net) return C->rdir[i].cur;
    return net;
}

static void
lw_rset(lw_ctx_t *C, uint32_t net, uint32_t cur, uint32_t cell)
{
    uint32_t i;
    for (i = 0; i < C->n_rdir; i++) {
        if (C->rdir[i].net == net) {
            C->rdir[i].cur = cur;
            C->rdir[i].cell = cell;
            return;
        }
    }
    if (C->n_rdir < LW_RDIR_MAX) {
        C->rdir[C->n_rdir].net = net;
        C->rdir[C->n_rdir].cur = cur;
        C->rdir[C->n_rdir].cell = cell;
        C->n_rdir++;
    }
}

/* ---- Helpers ---- */

static const char *
lw_text(const lw_ctx_t *C, uint32_t nidx)
{
    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return "";
    return C->P->lex->strs + C->P->nodes[nidx].d.text.off;
}

static uint16_t
lw_tlen(const lw_ctx_t *C, uint32_t nidx)
{
    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return 0;
    return C->P->nodes[nidx].d.text.len;
}

static uint32_t
lw_width(const lw_ctx_t *C, uint32_t nidx)
{
    if (nidx < C->nvals && C->wv[nidx].resolved)
        return C->wv[nidx].width;
    return 1;  /* default 1-bit */
}

/* ---- ceil(log2(n)) — address bits for n elements ---- */

static uint32_t
lw_clog2(uint32_t n)
{
    uint32_t b = 0;
    if (n == 0) return 1;
    n--;
    while (n > 0) { n >>= 1; b++; }
    return b ? b : 1;
}

/* ---- Register a memory (array with second range) ----
 * logic [7:0] mem [0:15] → mems[] entry.
 * Returns mem index (1-based) or 0 if not a memory. */

static uint32_t
lw_mmem(lw_ctx_t *C, uint32_t decl_n, uint32_t data_w)
{
    const tk_node_t *n = &C->P->nodes[decl_n];
    uint32_t ch, ident_n = 0, range_n = 0;
    uint32_t mi;

    /* Walk children: expect TYPE_SPEC, IDENT, then RANGE (array dim) */
    ch = n->first_child;
    KA_GUARD(gm, 20);
    while (ch && gm--) {
        if (C->P->nodes[ch].type == TK_AST_IDENT && !ident_n)
            ident_n = ch;
        else if (C->P->nodes[ch].type == TK_AST_RANGE && ident_n)
            range_n = ch;  /* second range = array dimension */
        ch = C->P->nodes[ch].next_sib;
    }
    if (!ident_n || !range_n) return 0;

    /* Extract depth from range [lo:hi] or [hi:lo] */
    {
        uint32_t hi_n = C->P->nodes[range_n].first_child;
        uint32_t lo_n = hi_n ? C->P->nodes[hi_n].next_sib : 0;
        int64_t hi_v, lo_v;
        uint32_t depth;

        if (!hi_n || !lo_n) return 0;
        if (hi_n >= C->nvals || lo_n >= C->nvals) return 0;
        if (!C->cv[hi_n].valid || !C->cv[lo_n].valid) return 0;

        hi_v = C->cv[hi_n].val;
        lo_v = C->cv[lo_n].val;
        depth = (uint32_t)(hi_v >= lo_v ? hi_v - lo_v + 1 : lo_v - hi_v + 1);

        if (C->M->n_mem >= RT_MAX_MEMS) return 0;
        mi = C->M->n_mem++;

        C->M->mems[mi].name_off = C->M->nets[0].name_off; /* temp */
        C->M->mems[mi].name_len = 0;
        C->M->mems[mi].data_w   = data_w;
        C->M->mems[mi].depth    = depth;
        C->M->mems[mi].addr_w   = lw_clog2(depth);

        /* Intern the memory name */
        {
            const char *nm = lw_text(C, ident_n);
            uint16_t nl = lw_tlen(C, ident_n);
            uint32_t so = C->M->str_len;
            if (so + nl + 1 <= C->M->str_max) {
                memcpy(C->M->strs + so, nm, nl);
                C->M->strs[so + nl] = '\0';
                C->M->str_len += nl + 1;
            }
            C->M->mems[mi].name_off = so;
            C->M->mems[mi].name_len = nl;
        }
    }
    return mi + 1;  /* 1-based */
}

/* ---- Check if an IDENT names a registered memory ----
 * Returns mem index (0-based) or -1 */

static int
lw_ismem(const lw_ctx_t *C, uint32_t ident_n)
{
    const char *nm;
    uint16_t nl;
    uint32_t i;

    if (ident_n == 0) return -1;
    nm = lw_text(C, ident_n);
    nl = lw_tlen(C, ident_n);

    for (i = 0; i < C->M->n_mem; i++) {
        if (C->M->mems[i].name_len == nl &&
            memcmp(C->M->strs + C->M->mems[i].name_off, nm, nl) == 0)
            return (int)i;
    }
    return -1;
}

/* ---- Find or create a net for an AST identifier ----
 * Deduplicates by name: if a net with the same name already
 * exists, reuse it. Otherwise the same signal gets multiple
 * nets and the RTL looks like a phonebook with duplicate
 * entries for everyone called Smith. */

static uint32_t
lw_fnet(lw_ctx_t *C, uint32_t ast_id)
{
    const char *nm;
    uint16_t nlen;
    uint32_t w, ni, i;

    if (ast_id == 0 || ast_id >= C->nmsz) return 0;

    /* Already mapped from this exact AST node? */
    if (C->nmap[ast_id] != 0) return C->nmap[ast_id];

    nm   = lw_text(C, ast_id);
    nlen = lw_tlen(C, ast_id);
    w    = lw_width(C, ast_id);

    /* Scan nets in current module scope for name match */
    for (i = C->net_lo ? C->net_lo : 1; i < C->M->n_net; i++) {
        const rt_net_t *n = &C->M->nets[i];
        if (n->name_len == nlen &&
            memcmp(C->M->strs + n->name_off, nm, nlen) == 0) {
            C->nmap[ast_id] = i;
            return i;
        }
    }

    /* Create new net */
    ni = rt_anet(C->M, nm, nlen, w, 0, C->radix);
    C->nmap[ast_id] = ni;
    return ni;
}

/* ---- Lower expression to net index ----
 * Returns the net index that carries the expression result.
 * Creates cells and intermediate nets as needed. */

static uint32_t
lw_expr(lw_ctx_t *C, uint32_t nidx)
{
    const tk_node_t *n;

    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return 0;
    n = &C->P->nodes[nidx];

    switch ((int)n->type) {
    case TK_AST_IDENT:
        return lw_fnet(C, nidx);

    case TK_AST_INT_LIT:
    {
        /* Width: use cv width if available, else 32 (SV default) */
        uint32_t cw = 32;
        int64_t val = 0;
        uint32_t onet, ci;
        if (nidx < C->nvals && C->cv[nidx].valid) {
            val = C->cv[nidx].val;
            if (C->cv[nidx].width > 0) cw = C->cv[nidx].width;
        }
        onet = rt_anet(C->M, "const", 5, cw, 0, C->radix);
        ci = rt_acell(C->M, RT_CONST, onet, NULL, 0, cw);
        if (ci > 0 && ci < C->M->n_cell)
            C->M->cells[ci].param = val;
        return onet;
    }

    case TK_AST_BINARY_OP:
    {
        uint32_t c1 = n->first_child;
        uint32_t c2 = c1 ? C->P->nodes[c1].next_sib : 0;
        uint32_t l, r, onet, ow;
        const char *op;
        rt_ctype_t ct;
        uint32_t ins[2];
        int is_cmp;

        if (c1 == 0 || c2 == 0) return 0;

        l = lw_expr(C, c1);
        r = lw_expr(C, c2);
        if (l == 0 || r == 0) return 0;

        /* Map operator to cell type */
        op = C->P->lex->strs +
            C->P->lex->ops[n->op].chars_off;

        ct = RT_ASSIGN;
        is_cmp = 0;
        if (strcmp(op, "+") == 0)       ct = RT_ADD;
        else if (strcmp(op, "-") == 0)  ct = RT_SUB;
        else if (strcmp(op, "*") == 0)  ct = RT_MUL;
        else if (strcmp(op, "&") == 0)  ct = RT_AND;
        else if (strcmp(op, "|") == 0)  ct = RT_OR;
        else if (strcmp(op, "^") == 0)  ct = RT_XOR;
        else if (strcmp(op, "~^") == 0) ct = RT_XNOR;
        else if (strcmp(op, "^~") == 0) ct = RT_XNOR;
        else if (strcmp(op, "<<") == 0) ct = RT_SHL;
        else if (strcmp(op, ">>") == 0) ct = RT_SHR;
        else if (strcmp(op, ">>>") == 0) ct = RT_SHRA;
        else if (strcmp(op, "==") == 0) { ct = RT_EQ; is_cmp = 1; }
        else if (strcmp(op, "!=") == 0) { ct = RT_NE; is_cmp = 1; }
        else if (strcmp(op, "<") == 0)  { ct = RT_LT; is_cmp = 1; }
        else if (strcmp(op, "<=") == 0) { ct = RT_LE; is_cmp = 1; }
        else if (strcmp(op, ">") == 0)  { ct = RT_GT; is_cmp = 1; }
        else if (strcmp(op, ">=") == 0) { ct = RT_GE; is_cmp = 1; }
        else if (strcmp(op, "&&") == 0) { ct = RT_AND; is_cmp = 1; }
        else if (strcmp(op, "||") == 0) { ct = RT_OR;  is_cmp = 1; }

        /* Width from operand nets, not AST inference */
        if (is_cmp) {
            ow = 1;
        } else {
            uint32_t lw = l < C->M->n_net ? C->M->nets[l].width : 1;
            uint32_t rw = r < C->M->n_net ? C->M->nets[r].width : 1;
            ow = lw > rw ? lw : rw;
        }

        onet = rt_anet(C->M, "tmp", 3, ow, 0, C->radix);
        ins[0] = l; ins[1] = r;
        rt_acell(C->M, ct, onet, ins, 2, ow);
        return onet;
    }

    case TK_AST_UNARY_OP:
    {
        uint32_t c1 = n->first_child;
        uint32_t operand;
        const char *op;

        if (c1 == 0) return 0;
        operand = lw_expr(C, c1);
        if (operand == 0) return 0;

        op = C->P->lex->strs +
            C->P->lex->ops[n->op].chars_off;

        if (strcmp(op, "~") == 0) {
            /* Bitwise NOT: same width as operand */
            uint32_t ow = operand < C->M->n_net ?
                C->M->nets[operand].width : 1;
            uint32_t onet = rt_anet(C->M, "tmp", 3, ow, 0, C->radix);
            uint32_t ins[1] = { operand };
            rt_acell(C->M, RT_NOT, onet, ins, 1, ow);
            return onet;
        }
        if (strcmp(op, "!") == 0) {
            /* Logical NOT: always 1-bit */
            uint32_t onet = rt_anet(C->M, "tmp", 3, 1, 0, C->radix);
            uint32_t ins[1] = { operand };
            rt_acell(C->M, RT_NOT, onet, ins, 1, 1);
            return onet;
        }
        return operand;  /* +a is just a */
    }

    case TK_AST_INDEX:
    {
        /* Bit/part select: a[7:0], a[5], mem[addr]
         * INDEX(base, selector) where selector is:
         *   RANGE(hi, lo) → constant part-select
         *   INT_LIT       → constant bit-select
         *   expr          → dynamic select */
        uint32_t base_n = n->first_child;
        uint32_t sel_n  = base_n ? C->P->nodes[base_n].next_sib : 0;
        uint32_t bnet, onet, ow, ci2;
        uint32_t ins2[2];

        if (!base_n || !sel_n) return 0;
        bnet = lw_expr(C, base_n);
        if (bnet == 0) return 0;

        if (C->P->nodes[sel_n].type == TK_AST_RANGE) {
            /* Constant part-select: a[hi:lo] */
            uint32_t hi_n = C->P->nodes[sel_n].first_child;
            uint32_t lo_n = hi_n ? C->P->nodes[hi_n].next_sib : 0;
            int64_t hi_v, lo_v;

            if (hi_n && lo_n &&
                hi_n < C->nvals && C->cv[hi_n].valid &&
                lo_n < C->nvals && C->cv[lo_n].valid) {
                hi_v = C->cv[hi_n].val;
                lo_v = C->cv[lo_n].val;
                if (hi_v < lo_v) { int64_t t = hi_v; hi_v = lo_v; lo_v = t; }
                ow = (uint32_t)(hi_v - lo_v + 1);
                onet = rt_anet(C->M, "sel", 3, ow, 0, C->radix);
                ins2[0] = bnet;
                ci2 = rt_acell(C->M, RT_SELECT, onet, ins2, 1, ow);
                if (ci2 > 0 && ci2 < C->M->n_cell)
                    C->M->cells[ci2].param = (hi_v << 16) | (lo_v & 0xFFFF);
                return onet;
            }
            /* Non-constant range → dynamic (fall through below) */
        }

        if (C->P->nodes[sel_n].type == TK_AST_INT_LIT) {
            /* Constant bit-select: a[5] */
            int64_t bit = 0;
            if (sel_n < C->nvals && C->cv[sel_n].valid)
                bit = C->cv[sel_n].val;
            onet = rt_anet(C->M, "sel", 3, 1, 0, C->radix);
            ins2[0] = bnet;
            ci2 = rt_acell(C->M, RT_SELECT, onet, ins2, 1, 1);
            if (ci2 > 0 && ci2 < C->M->n_cell)
                C->M->cells[ci2].param = (bit << 16) | (bit & 0xFFFF);
            return onet;
        }

        /* Dynamic select or memory read */
        {
            uint32_t anet = lw_expr(C, sel_n);
            int mi = lw_ismem(C, base_n);
            if (anet == 0) return 0;

            if (mi >= 0) {
                /* Memory read port */
                ow = C->M->mems[mi].data_w;
                onet = rt_anet(C->M, "mrd", 3, ow, 0, C->radix);
                ins2[0] = anet;
                ci2 = rt_acell(C->M, RT_MEMRD, onet, ins2, 1, ow);
                if (ci2 > 0 && ci2 < C->M->n_cell)
                    C->M->cells[ci2].param = (int64_t)mi;
                return onet;
            }

            /* Regular dynamic select */
            {
                uint32_t dw = bnet < C->M->n_net ?
                    C->M->nets[bnet].width : 1;
                ow = lw_width(C, nidx);
                if (ow == 0) ow = 1;
                onet = rt_anet(C->M, "sel", 3, ow, 0, C->radix);
                ins2[0] = bnet; ins2[1] = anet;
                ci2 = rt_acell(C->M, RT_SELECT, onet, ins2, 2, ow);
                if (ci2 > 0 && ci2 < C->M->n_cell)
                    C->M->cells[ci2].param = (int64_t)dw;
                return onet;
            }
        }
    }

    case TK_AST_CONCAT:
    {
        /* {a, b, c} → CONCAT cell(s).
         * Walk children, lower each, sum widths.
         * ≤8 children → single RT_CONCAT.
         * >8 → chain of CONCAT cells (7 + acc). */
        uint32_t kids[64];
        uint32_t kwid[64];
        int nk = 0;
        uint32_t ch2 = n->first_child;
        uint32_t sumw = 0;

        KA_GUARD(gcat, 64);
        while (ch2 && gcat-- && nk < 64) {
            uint32_t kn = lw_expr(C, ch2);
            if (kn == 0) { ch2 = C->P->nodes[ch2].next_sib; continue; }
            kids[nk] = kn;
            kwid[nk] = kn < C->M->n_net ? C->M->nets[kn].width : 1;
            sumw += kwid[nk];
            nk++;
            ch2 = C->P->nodes[ch2].next_sib;
        }

        if (nk == 0) return 0;
        if (nk == 1) return kids[0];

        if (nk <= RT_MAX_PIN) {
            /* Single CONCAT cell */
            uint32_t cins[RT_MAX_PIN];
            int jj;
            uint32_t onet2;
            for (jj = 0; jj < nk; jj++) cins[jj] = kids[jj];
            onet2 = rt_anet(C->M, "cat", 3, sumw, 0, C->radix);
            rt_acell(C->M, RT_CONCAT, onet2, cins, (uint8_t)nk, sumw);
            return onet2;
        } else {
            /* Chain: groups of 7 + accumulator */
            uint32_t acc = 0, aw = 0;
            int pos = 0;
            while (pos < nk) {
                int grp = nk - pos;
                if (acc) grp = grp > 7 ? 7 : grp;
                else     grp = grp > 8 ? 8 : grp;

                uint32_t cins2[RT_MAX_PIN];
                uint32_t gw = 0;
                int jj, gi = 0;

                if (acc) { cins2[gi++] = acc; gw = aw; }
                for (jj = 0; jj < grp; jj++) {
                    cins2[gi++] = kids[pos + jj];
                    gw += kwid[pos + jj];
                }
                pos += grp;

                acc = rt_anet(C->M, "cat", 3, gw, 0, C->radix);
                rt_acell(C->M, RT_CONCAT, acc, cins2, (uint8_t)gi, gw);
                aw = gw;
            }
            return acc;
        }
    }

    case TK_AST_TERNARY:
    {
        /* a ? b : c -> MUX(sel=a, d0=c, d1=b) */
        uint32_t cond_n = n->first_child;
        uint32_t then_n = cond_n ? C->P->nodes[cond_n].next_sib : 0;
        uint32_t else_n = then_n ? C->P->nodes[then_n].next_sib : 0;
        uint32_t sel, d1, d0, onet, ow, dw0, dw1;
        uint32_t ins[3];

        if (!cond_n || !then_n || !else_n) return 0;

        sel = lw_expr(C, cond_n);
        d1  = lw_expr(C, then_n);
        d0  = lw_expr(C, else_n);

        /* Width from data operands, not AST inference */
        dw1 = d1 < C->M->n_net ? C->M->nets[d1].width : 1;
        dw0 = d0 < C->M->n_net ? C->M->nets[d0].width : 1;
        ow = dw1 > dw0 ? dw1 : dw0;

        onet = rt_anet(C->M, "tmp", 3, ow, 0, C->radix);
        ins[0] = sel; ins[1] = d0; ins[2] = d1;
        rt_acell(C->M, RT_MUX, onet, ins, 3, ow);
        return onet;
    }

    case TK_AST_CALL:
    {
        /* System function calls: $signed, $unsigned, $clog2.
         * For synthesis, $signed/$unsigned are passthrough —
         * they affect how comparisons interpret the bits,
         * not the bits themselves. The sign bit is already
         * in the MSB. Like telling the customs officer
         * "this is wine" vs "this is grape juice" — same
         * bottle, different paperwork. */
        const char *fn;
        uint32_t arg;

        if (n->d.text.len == 0) return 0;
        fn = C->P->lex->strs + n->d.text.off;
        arg = n->first_child;

        if (strcmp(fn, "$signed") == 0 ||
            strcmp(fn, "$unsigned") == 0 ||
            /* VHDL type casts: unsigned(x), signed(x),
             * std_logic_vector(x) — all passthrough for
             * synthesis. The bits don't change, only the
             * type system's opinion of them. */
            strcmp(fn, "unsigned") == 0 ||
            strcmp(fn, "signed") == 0 ||
            strcmp(fn, "std_logic_vector") == 0) {
            return arg ? lw_expr(C, arg) : 0;
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ---- Lower an assignment (blocking or non-blocking) ---- */

static void
lw_asgn(lw_ctx_t *C, uint32_t nidx, int is_reg)
{
    const tk_node_t *n;
    uint32_t lhs_n, rhs_n, lhs, rhs, w;
    uint32_t ins[2];

    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return;
    n = &C->P->nodes[nidx];

    lhs_n = n->first_child;
    rhs_n = lhs_n ? C->P->nodes[lhs_n].next_sib : 0;
    if (!lhs_n || !rhs_n) return;

    /* Memory write: LHS is INDEX into a memory array */
    if (C->P->nodes[lhs_n].type == TK_AST_INDEX) {
        uint32_t base_n = C->P->nodes[lhs_n].first_child;
        uint32_t idx_n  = base_n ? C->P->nodes[base_n].next_sib : 0;
        int mi;

        if (base_n && (mi = lw_ismem(C, base_n)) >= 0) {
            uint32_t anet, dnet, onet;
            uint32_t mw = C->M->mems[mi].data_w;
            uint32_t ci2;

            /* Both sides of mem[addr] <= data need evaluating
             * before we can build the write cell */
            anet = idx_n ? lw_expr(C, idx_n) : 0;
            dnet = lw_expr(C, rhs_n);
            if (anet == 0 || dnet == 0) return;

            /* If we're inside an if(we) block, the condition
             * net rides along as a third input so the emitter
             * can reconstruct the conditional write */
            onet = rt_anet(C->M, "mwr", 3, mw, 0, C->radix);
            {
                uint32_t mins[3];
                uint8_t mn = 2;
                mins[0] = anet;
                mins[1] = dnet;
                if (C->mem_we > 0) {
                    mins[2] = C->mem_we;
                    mn = 3;
                }
                ci2 = rt_acell(C->M, RT_MEMWR, onet, mins, mn, mw);
            }
            if (ci2 > 0 && ci2 < C->M->n_cell)
                C->M->cells[ci2].param = (int64_t)mi;
            return;
        }
    }

    /* Partial bit assignment: x[hi:lo] <= val
     * Read-modify-write: splice new bits into current value.
     * {current[W-1:hi+1], val, current[lo-1:0]} → x */
    if (C->P->nodes[lhs_n].type == TK_AST_INDEX) {
        uint32_t base_n = C->P->nodes[lhs_n].first_child;
        uint32_t sel_n  = base_n ? C->P->nodes[base_n].next_sib : 0;

        if (base_n && sel_n &&
            C->P->nodes[sel_n].type == TK_AST_RANGE) {
            uint32_t hi_n = C->P->nodes[sel_n].first_child;
            uint32_t lo_n = hi_n ? C->P->nodes[hi_n].next_sib : 0;

            if (hi_n && lo_n &&
                hi_n < C->nvals && C->cv[hi_n].valid &&
                lo_n < C->nvals && C->cv[lo_n].valid) {
                int64_t hi_v = C->cv[hi_n].val;
                int64_t lo_v = C->cv[lo_n].val;
                uint32_t base_net, cur, rhs2, result;
                uint32_t bw;

                if (hi_v < lo_v) { int64_t t = hi_v; hi_v = lo_v; lo_v = t; }

                base_net = lw_fnet(C, base_n);
                if (base_net == 0) return;
                bw = C->M->nets[base_net].width;
                cur = lw_rcur(C, base_net);
                rhs2 = lw_expr(C, rhs_n);
                if (rhs2 == 0) return;

                /* Build concatenation: {upper, val, lower} */
                {
                    uint32_t cins[3];
                    uint8_t nc = 0;
                    uint32_t slice_ci;

                    /* Lower bits [lo-1:0] */
                    if (lo_v > 0) {
                        uint32_t lo_net = rt_anet(C->M, "slo", 3,
                            (uint32_t)lo_v, 0, C->radix);
                        uint32_t si[1] = { cur };
                        slice_ci = rt_acell(C->M, RT_SELECT, lo_net, si, 1,
                            (uint32_t)lo_v);
                        if (slice_ci > 0 && slice_ci < C->M->n_cell)
                            C->M->cells[slice_ci].param =
                                (((int64_t)lo_v - 1) << 16) | 0;
                        cins[nc++] = lo_net;
                    }

                    /* New value */
                    cins[nc++] = rhs2;

                    /* Upper bits [W-1:hi+1] */
                    if ((uint32_t)(hi_v + 1) < bw) {
                        uint32_t uw = bw - (uint32_t)(hi_v + 1);
                        uint32_t hi_net = rt_anet(C->M, "shi", 3,
                            uw, 0, C->radix);
                        uint32_t si[1] = { cur };
                        slice_ci = rt_acell(C->M, RT_SELECT, hi_net, si, 1, uw);
                        if (slice_ci > 0 && slice_ci < C->M->n_cell)
                            C->M->cells[slice_ci].param =
                                (((int64_t)bw - 1) << 16) | (hi_v + 1);
                        cins[nc++] = hi_net;
                    }

                    /* Concatenate into full-width result */
                    result = rt_anet(C->M, "rmw", 3, bw, 0, C->radix);
                    if (nc == 1) {
                        uint32_t ai[1] = { cins[0] };
                        rt_acell(C->M, RT_ASSIGN, result, ai, 1, bw);
                    } else {
                        rt_acell(C->M, RT_CONCAT, result, cins, nc, bw);
                    }

                    /* Assign back to base net */
                    if (is_reg) C->M->nets[base_net].is_reg = 1;
                    {
                        uint32_t tmp = rt_anet(C->M, "seq", 3, bw, 0, C->radix);
                        uint32_t ai[1] = { result };
                        uint32_t ci3 = rt_acell(C->M, RT_ASSIGN, tmp, ai, 1, bw);
                        lw_rset(C, base_net, tmp, ci3);
                    }
                }
                return;
            }
        }
    }

    lhs = lw_fnet(C, lhs_n);
    rhs = lw_expr(C, rhs_n);
    if (lhs == 0 || rhs == 0) return;

    w = C->M->nets[lhs].width;

    if (is_reg) {
        C->M->nets[lhs].is_reg = 1;
    }

    /* Inside always block: drive temp, let flush rewire.
     * Outside: drive target directly (continuous assign). */
    if (C->n_rdir > 0 || is_reg) {
        uint32_t tmp = rt_anet(C->M, "seq", 3, w, 0, C->radix);
        uint32_t ci2;
        ins[0] = rhs;
        ci2 = rt_acell(C->M, RT_ASSIGN, tmp, ins, 1, w);
        lw_rset(C, lhs, tmp, ci2);
    } else {
        ins[0] = rhs;
        rt_acell(C->M, RT_ASSIGN, lhs, ins, 1, w);
    }
}

/* ---- Forward declarations for mutual recursion ---- */
static void lw_stms(lw_ctx_t *C, uint32_t nidx, int is_reg);

/* lw_tgt / lw_rhs removed — replaced by lw_mget which handles
 * multi-assignment bodies (begin/end with multiple assigns).
 * The old single-target helpers couldn't handle normal ALU
 * patterns like case(op) with result + carry_out per branch. */

/* ---- Multi-assignment target/rhs collection ----
 * A case item body may contain multiple assignments inside
 * a begin/end block. This is the normal pattern for any ALU:
 *
 *   3'b000: begin
 *       result = {d_sum[3], ...};
 *       carry_out = d_carry[4];
 *   end
 *
 * lw_tgt only handles single assignments and returns 0 for
 * multi-assign bodies, causing lw_cmux to fall through to flat
 * lowering — which emits every branch as concurrent drivers.
 * The Sumerians had no word for "multi-driver conflict" because
 * they were smart enough not to create one.
 *
 * lw_mget collects up to `max` (target, rhs_node) pairs from
 * a statement or begin/end block. Returns count found. */

typedef struct { uint32_t tgt; uint32_t rhs_n; } lw_pair_t;

static int
lw_mget(lw_ctx_t *C, uint32_t nidx, lw_pair_t *out, int max)
{
    const tk_node_t *n;
    uint32_t ch, lhs;
    int cnt = 0;

    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return 0;
    n = &C->P->nodes[nidx];

    if (n->type == TK_AST_NONBLOCK || n->type == TK_AST_BLOCK_ASSIGN) {
        lhs = n->first_child;
        if (lhs && C->P->nodes[lhs].type == TK_AST_IDENT && cnt < max) {
            out[cnt].tgt  = lw_fnet(C, lhs);
            out[cnt].rhs_n = lhs ? C->P->nodes[lhs].next_sib : 0;
            if (out[cnt].tgt && out[cnt].rhs_n) cnt++;
        }
        return cnt;
    }

    if (n->type == TK_AST_BEGIN_END || n->type == TK_AST_CASE_ITEM) {
        /* BEGIN_END: all children are statements.
         * CASE_ITEM: first child is value, rest are statements.
         * Skip the value child for CASE_ITEM. */
        ch = n->first_child;
        if (n->type == TK_AST_CASE_ITEM && ch) {
            /* Skip value expression(s) — walk past non-assign children */
            KA_GUARD(gs, 64);
            while (ch && gs--) {
                if (C->P->nodes[ch].type == TK_AST_NONBLOCK ||
                    C->P->nodes[ch].type == TK_AST_BLOCK_ASSIGN ||
                    C->P->nodes[ch].type == TK_AST_BEGIN_END ||
                    C->P->nodes[ch].type == TK_AST_IF)
                    break;
                ch = C->P->nodes[ch].next_sib;
            }
        }
        KA_GUARD(gm, 64);
        while (ch && gm-- && cnt < max) {
            const tk_node_t *cn = &C->P->nodes[ch];
            if (cn->type == TK_AST_NONBLOCK ||
                cn->type == TK_AST_BLOCK_ASSIGN) {
                lhs = cn->first_child;
                if (lhs && C->P->nodes[lhs].type == TK_AST_IDENT) {
                    out[cnt].tgt  = lw_fnet(C, lhs);
                    out[cnt].rhs_n = lhs ? C->P->nodes[lhs].next_sib : 0;
                    if (out[cnt].tgt && out[cnt].rhs_n) cnt++;
                }
                /* Indexed LHS: mem_rdata_q[14:12] <= ... */
                else if (lhs && C->P->nodes[lhs].type == TK_AST_INDEX) {
                    uint32_t base = C->P->nodes[lhs].first_child;
                    if (base && C->P->nodes[base].type == TK_AST_IDENT) {
                        out[cnt].tgt = lw_fnet(C, base);
                        out[cnt].rhs_n = lhs ? C->P->nodes[lhs].next_sib : 0;
                        if (out[cnt].tgt && out[cnt].rhs_n) cnt++;
                    }
                }
            } else if (cn->type == TK_AST_BEGIN_END) {
                /* Unwrap nested begin/end */
                int sub = lw_mget(C, ch, out + cnt, max - cnt);
                cnt += sub;
            }
            /* Don't recurse into nested CASE/IF — those get their
             * own MUX chains via lw_stms. Collecting their targets
             * here causes the outer case to ALSO drive those nets,
             * creating multi-driver conflicts. */
            ch = C->P->nodes[ch].next_sib;
        }
        return cnt;
    }

    return 0;
}

/* ---- Lower if/else to MUX ----
 * IF node children: cond, then-stmt, [else-stmt]
 * Handles both single-assignment and multi-assignment bodies.
 * Multi-assign: build a MUX per target, same as lw_cmux. */

static void
lw_ifmux(lw_ctx_t *C, uint32_t nidx, int is_reg)
{
    const tk_node_t *n;
    uint32_t cond_n, then_n, else_n;

    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return;
    n = &C->P->nodes[nidx];

    cond_n = n->first_child;
    then_n = cond_n ? C->P->nodes[cond_n].next_sib : 0;
    else_n = then_n ? C->P->nodes[then_n].next_sib : 0;

    if (!cond_n || !then_n) return;

    /* Collect targets from then-branch */
    {
        lw_pair_t tpairs[16], epairs[16];
        int tnp = lw_mget(C, then_n, tpairs, 16);
        int enp = else_n ? lw_mget(C, else_n, epairs, 16) : 0;
        int ti;

        if (tnp == 0) goto flat;

        /* For else-if chains, recurse first then wrap */
        if (else_n && C->P->nodes[else_n].type == TK_AST_IF) {
            uint32_t sel;
            lw_ifmux(C, else_n, is_reg);
            sel = lw_expr(C, cond_n);
            if (sel == 0) goto flat;

            for (ti = 0; ti < tnp; ti++) {
                uint32_t tgt = tpairs[ti].tgt;
                uint32_t d1  = lw_expr(C, tpairs[ti].rhs_n);
                uint32_t w, ins[3];
                if (!tgt || !d1) continue;
                w = tgt < C->M->n_net ? C->M->nets[tgt].width : 1;
                if (is_reg) C->M->nets[tgt].is_reg = 1;
                {
                    uint32_t hold = lw_rcur(C, tgt);
                    uint32_t tmp = rt_anet(C->M, "seq", 3, w, 0, C->radix);
                    uint32_t ci2;
                    ins[0] = sel; ins[1] = hold; ins[2] = d1;
                    ci2 = rt_acell(C->M, RT_MUX, tmp, ins, 3, w);
                    lw_rset(C, tgt, tmp, ci2);
                }
            }
            return;
        }

        /* Normal if/else or bare if */
        {
            uint32_t sel = lw_expr(C, cond_n);
            if (sel == 0) goto flat;

            for (ti = 0; ti < tnp; ti++) {
                uint32_t tgt = tpairs[ti].tgt;
                uint32_t d1  = lw_expr(C, tpairs[ti].rhs_n);
                uint32_t d0  = 0;
                uint32_t w, ins[3];
                int ek;

                if (!tgt || !d1) continue;
                w = tgt < C->M->n_net ? C->M->nets[tgt].width : 1;

                /* Find matching else-branch RHS for this target */
                for (ek = 0; ek < enp; ek++) {
                    if (epairs[ek].tgt == tgt) {
                        d0 = lw_expr(C, epairs[ek].rhs_n);
                        break;
                    }
                }
                /* No else: hold = current value (may be from
                 * a prior assignment in the same block) */
                if (d0 == 0) d0 = lw_rcur(C, tgt);

                if (is_reg) C->M->nets[tgt].is_reg = 1;

                /* Drive temp, record in redirect table.
                 * Flush at end of always block rewires the
                 * LAST temp's cell to drive tgt directly. */
                {
                    uint32_t tmp = rt_anet(C->M, "seq", 3, w, 0, C->radix);
                    uint32_t ci2;
                    ins[0] = sel; ins[1] = d0; ins[2] = d1;
                    ci2 = rt_acell(C->M, RT_MUX, tmp, ins, 3, w);
                    lw_rset(C, tgt, tmp, ci2);
                }
            }
        }
        return;
    }

flat:
    /* Pattern didn't match: lower all children flat.
     * Set mem_we so memory writes inside the if body
     * pick up the condition as their write-enable. */
    {
        uint32_t saved_we = C->mem_we;
        uint32_t cnet = cond_n ? lw_expr(C, cond_n) : 0;
        if (cnet > 0)
            C->mem_we = cnet;

        if (then_n) lw_stms(C, then_n, is_reg);
        C->mem_we = saved_we;
        if (else_n) lw_stms(C, else_n, is_reg);
    }
}

/* ---- Lower case to MUX chain ----
 * CASE node children: selector_expr, case_item, case_item, ...
 * Each CASE_ITEM: value_expr(s), body_stmt
 * Default item has no value_expr — just body_stmt.
 *
 * Each case item body can contain MULTIPLE assignments inside
 * a begin/end block (the normal ALU pattern). We collect all
 * target nets across all items and build a separate MUX chain
 * for each target. Like a Sumerian merchant sorting his goods
 * into separate baskets before pricing them — you don't throw
 * copper ingots and barley into the same pile.
 *
 * Right-to-left priority: default at bottom, each item wraps
 * it in a MUX keyed on (sel == val). */

/* Forward declaration for BEGIN_END handler */
static void lw_cmux_d(lw_ctx_t *C, uint32_t nidx, int is_reg,
                      const lw_pair_t *pdflts, int npdfl);

static void
lw_cmux(lw_ctx_t *C, uint32_t nidx, int is_reg)
{
    lw_cmux_d(C, nidx, is_reg, NULL, 0);
}

/* ---- Lower case to MUX chain (with optional pre-case defaults) ----
 * pdflts/npdfl: assignments before the case in the always_comb
 * block that establish default values. These become the initial
 * accumulator for targets not covered by the case's default item.
 *
 * Without this, `carry_out = 1'b0; case(op) ...` creates two
 * drivers on carry_out — the pre-case ASSIGN and the case MUX.
 * With this, the pre-case value feeds into the MUX chain as the
 * default, and only one cell drives carry_out. */

static void
lw_cmux_d(lw_ctx_t *C, uint32_t nidx, int is_reg,
          const lw_pair_t *pdflts, int npdfl)
{
    const tk_node_t *n;
    uint32_t sel_n, ch;
    uint32_t sel_net;
    uint32_t items[64];
    uint32_t dflt = 0;
    int nitm = 0;

    uint32_t tgts[16];
    int ntgt = 0;

    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return;
    n = &C->P->nodes[nidx];

    sel_n = n->first_child;
    if (!sel_n) goto flat_c;

    /* Collect case items */
    ch = C->P->nodes[sel_n].next_sib;
    KA_GUARD(gc, 256);
    while (ch && gc--) {
        if (C->P->nodes[ch].type == TK_AST_CASE_ITEM) {
            uint32_t fc = C->P->nodes[ch].first_child;
            uint32_t sc = fc ? C->P->nodes[fc].next_sib : 0;
            if (!sc) {
                dflt = ch;
            } else if (nitm < 64) {
                items[nitm++] = ch;
            }
        }
        ch = C->P->nodes[ch].next_sib;
    }

    if (nitm == 0) goto flat_c;

    /* ---- Detect case(1'b1) priority encoder pattern ----
     * case (1'b1)
     *   cond_a: x = 1;
     *   cond_b: x = 2;
     * endcase
     *
     * Each item's "value" is actually a boolean condition.
     * Lower as an if/elsif chain: first match wins.
     * PicoRV32 uses this for instruction decoding — it's
     * the reason half the internet's RISC-V cores exist. */
    /* Detect case(1'b1) / case(1) priority encoder.
     * The selector might be INT_LIT or REPLICATE (parser quirk
     * where {1{...}} gets the wrong AST type). Check both. */
    {
        int is_one = 0;
        if (C->P->nodes[sel_n].type == TK_AST_INT_LIT) {
            const char *sv = C->P->lex->strs + C->P->nodes[sel_n].d.text.off;
            uint16_t sl = C->P->nodes[sel_n].d.text.len;
            if (sl == 4 && memcmp(sv, "1'b1", 4) == 0) is_one = 1;
            if (sl == 1 && sv[0] == '1') is_one = 1;
            if (sl == 4 && memcmp(sv, "1'h1", 4) == 0) is_one = 1;
        }
        /* REPLICATE with a single 1-bit child that's 1 */
        if (C->P->nodes[sel_n].type == TK_AST_REPLICATE) is_one = 1;

        if (is_one && nitm > 0) {
            /* Collect all targets across all items */
            {
                lw_pair_t pairs[16];
                int np, k;
                /* Try each item until we find one with targets.
                 * Some items may have empty bodies after debug
                 * macro stripping. */
                {
                    int ii;
                    np = 0;
                    for (ii = 0; ii < nitm && np == 0 && ii < 8; ii++)
                        np = lw_mget(C, items[ii], pairs, 16);
                }
                if (np == 0) goto flat_c;
                for (k = 0; k < np && ntgt < 16; k++)
                    tgts[ntgt++] = pairs[k].tgt;
            }
            /* Also add targets from pre-case defaults */
            {
                int pi;
                for (pi = 0; pi < npdfl; pi++) {
                    int found = 0, k;
                    for (k = 0; k < ntgt; k++)
                        if (tgts[k] == pdflts[pi].tgt) { found = 1; break; }
                    if (!found && ntgt < 16)
                        tgts[ntgt++] = pdflts[pi].tgt;
                }
            }

            /* Build per-target MUX chains using item values as conditions.
             * Last item = lowest priority, first = highest. */
            {
                int ti;
                for (ti = 0; ti < ntgt; ti++) {
                    uint32_t tgt = tgts[ti];
                    uint32_t w = tgt < C->M->n_net ?
                        C->M->nets[tgt].width : 1;
                    uint32_t acc;
                    int j;

                    /* Default value: explicit default, pre-case, or hold */
                    acc = lw_rcur(C, tgt);
                    if (dflt) {
                        lw_pair_t dp[16];
                        int dn = lw_mget(C, dflt, dp, 16);
                        int dk;
                        for (dk = 0; dk < dn; dk++) {
                            if (dp[dk].tgt == tgt) {
                                uint32_t dr = lw_expr(C, dp[dk].rhs_n);
                                if (dr) acc = dr;
                                break;
                            }
                        }
                    }
                    if (acc == tgt && pdflts) {
                        int pi;
                        for (pi = 0; pi < npdfl; pi++) {
                            if (pdflts[pi].tgt == tgt) {
                                uint32_t pr = lw_expr(C, pdflts[pi].rhs_n);
                                if (pr) acc = pr;
                                break;
                            }
                        }
                    }
                    if (acc == 0) continue;

                    /* Build MUX chain: each item's value is the condition */
                    for (j = nitm - 1; j >= 0; j--) {
                        uint32_t item = items[j];
                        uint32_t cond_n = C->P->nodes[item].first_child;
                        uint32_t cond_net, body_net, ins[3];
                        lw_pair_t bp[16];
                        int bn, bk;

                        if (!cond_n) continue;

                        /* Condition: the case item "value" expression */
                        cond_net = lw_expr(C, cond_n);
                        if (cond_net == 0) continue;

                        /* Body: find this target's RHS */
                        bn = lw_mget(C, item, bp, 16);
                        body_net = 0;
                        for (bk = 0; bk < bn; bk++) {
                            if (bp[bk].tgt == tgt) {
                                body_net = lw_expr(C, bp[bk].rhs_n);
                                break;
                            }
                        }
                        if (body_net == 0) continue;

                        /* MUX: cond true → body, else → acc */
                        ins[0] = cond_net; ins[1] = acc; ins[2] = body_net;
                        if (j == 0) {
                            uint32_t out_n = rt_anet(C->M, "seq", 3, w, 0, C->radix);
                            uint32_t ci3 = rt_acell(C->M, RT_MUX, out_n, ins, 3, w);
                            if (is_reg) C->M->nets[tgt].is_reg = 1;
                            lw_rset(C, tgt, out_n, ci3);
                        } else {
                            acc = rt_anet(C->M, "pmx", 3, w, 0, C->radix);
                            rt_acell(C->M, RT_MUX, acc, ins, 3, w);
                        }
                    }
                }
            }
            return;
        }
    }

    /* Collect targets from first case item.
     * Pass the CASE_ITEM node itself — lw_mget knows how to
     * skip the value expression and collect the assignments. */
    {
        lw_pair_t pairs[16];
        int np, k;

        np = lw_mget(C, items[0], pairs, 16);
        if (np == 0) goto flat_c;

        for (k = 0; k < np && ntgt < 16; k++)
            tgts[ntgt++] = pairs[k].tgt;
    }

    /* Also add targets from pre-case defaults that aren't
     * already in the case items (e.g. carry_out = 1'b0) */
    {
        int pi;
        for (pi = 0; pi < npdfl; pi++) {
            int found = 0, k;
            for (k = 0; k < ntgt; k++) {
                if (tgts[k] == pdflts[pi].tgt) { found = 1; break; }
            }
            if (!found && ntgt < 16)
                tgts[ntgt++] = pdflts[pi].tgt;
        }
    }

    sel_net = lw_expr(C, sel_n);
    if (sel_net == 0) goto flat_c;

    /* Build a MUX chain for EACH target net */
    {
        int ti;
        for (ti = 0; ti < ntgt; ti++) {
            uint32_t tgt = tgts[ti];
            uint32_t w = tgt < C->M->n_net ? C->M->nets[tgt].width : 1;
            uint32_t acc;
            int j;

            /* Default value priority:
             * 1. Explicit default case item for this target
             * 2. Pre-case assignment for this target
             * 3. Hold (tgt itself) */
            acc = tgt;

            if (dflt) {
                lw_pair_t dpairs[16];
                int dnp = lw_mget(C, dflt, dpairs, 16);
                int dk;
                for (dk = 0; dk < dnp; dk++) {
                    if (dpairs[dk].tgt == tgt) {
                        uint32_t drhs = lw_expr(C, dpairs[dk].rhs_n);
                        if (drhs) acc = drhs;
                        break;
                    }
                }
            }

            /* If no explicit default, try pre-case assignment */
            if (acc == tgt && pdflts) {
                int pi;
                for (pi = 0; pi < npdfl; pi++) {
                    if (pdflts[pi].tgt == tgt) {
                        uint32_t prhs = lw_expr(C, pdflts[pi].rhs_n);
                        if (prhs) acc = prhs;
                        break;
                    }
                }
            }

            if (acc == 0) continue;

            /* Build MUX chain */
            for (j = nitm - 1; j >= 0; j--) {
                uint32_t item = items[j];
                uint32_t val_n = C->P->nodes[item].first_child;
                uint32_t val_net, body_net, cmp_net, ins[3];
                uint32_t cmp_ins[2];
                lw_pair_t bpairs[16];
                int bnp, bk;

                if (!val_n) continue;

                /* Collect targets from the CASE_ITEM node */
                bnp = lw_mget(C, item, bpairs, 16);
                body_net = 0;
                for (bk = 0; bk < bnp; bk++) {
                    if (bpairs[bk].tgt == tgt) {
                        body_net = lw_expr(C, bpairs[bk].rhs_n);
                        break;
                    }
                }
                if (body_net == 0) continue;

                val_net = lw_expr(C, val_n);
                if (val_net == 0) continue;

                cmp_net = rt_anet(C->M, "ceq", 3, 1, 0, C->radix);
                cmp_ins[0] = sel_net;
                cmp_ins[1] = val_net;
                rt_acell(C->M, RT_EQ, cmp_net, cmp_ins, 2, 1);

                ins[0] = cmp_net; ins[1] = acc; ins[2] = body_net;
                if (j == 0) {
                    uint32_t out_n = rt_anet(C->M, "seq", 3, w, 0, C->radix);
                    uint32_t ci3 = rt_acell(C->M, RT_MUX, out_n, ins, 3, w);
                    if (is_reg) C->M->nets[tgt].is_reg = 1;
                    lw_rset(C, tgt, out_n, ci3);
                } else {
                    acc = rt_anet(C->M, "cmx", 3, w, 0, C->radix);
                    rt_acell(C->M, RT_MUX, acc, ins, 3, w);
                }
            }
        }
    }
    return;

flat_c:
    printf("takahe: warning: case at node %u fell through to flat lowering\n",
           nidx);
    {
        uint32_t cc = C->P->nodes[nidx].first_child;
        KA_GUARD(gf, 10000);
        while (cc && gf--) {
            lw_stms(C, cc, is_reg);
            cc = C->P->nodes[cc].next_sib;
        }
    }
}

/* ---- Lower statements in an always block ---- */

static void
lw_stms(lw_ctx_t *C, uint32_t nidx, int is_reg)
{
    uint32_t c;

    if (nidx == 0 || KA_CHK(nidx, C->P->n_node)) return;

    /* Skip dead nodes */
    if (C->P->nodes[nidx].type == TK_AST_COUNT) return;

    switch ((int)C->P->nodes[nidx].type) {
    case TK_AST_NONBLOCK:
        lw_asgn(C, nidx, 1);
        break;

    case TK_AST_BLOCK_ASSIGN:
        lw_asgn(C, nidx, is_reg);
        break;

    case TK_AST_IF:
        lw_ifmux(C, nidx, is_reg);
        break;

    case TK_AST_CASE:
        lw_cmux(C, nidx, is_reg);
        break;

    case TK_AST_CASE_ITEM:
    {
        c = C->P->nodes[nidx].first_child;
        KA_GUARD(g2a, 10000);
        while (c && g2a--) {
            lw_stms(C, c, is_reg);
            c = C->P->nodes[c].next_sib;
        }
        break;
    }


    case TK_AST_BEGIN_END:
    {
        /* Detect "assigns then case" pattern in always_comb.
         * Pre-case assignments establish defaults. If we emit
         * them as standalone ASSIGNs AND the case also drives
         * the same net via a MUX chain, we get multi-driver
         * conflicts. Like two scribes writing on the same
         * tablet — only one can hold the stylus.
         *
         * Fix: scan ahead for a CASE child. Collect pre-case
         * assignments and feed them as defaults to lw_cmux.
         * Only emit pre-case assigns for targets NOT in the case. */
        uint32_t case_n = 0;
        lw_pair_t dflts[16];
        int ndflt = 0;

        /* Scan children: collect assigns, find case */
        c = C->P->nodes[nidx].first_child;
        KA_GUARD(g2b, 10000);
        while (c && g2b--) {
            if (C->P->nodes[c].type == TK_AST_CASE) {
                case_n = c;
                break;
            }
            c = C->P->nodes[c].next_sib;
        }

        if (case_n) {
            /* Collect pre-case assigns as defaults */
            c = C->P->nodes[nidx].first_child;
            KA_GUARD(g2c, 100);
            while (c && g2c-- && c != case_n) {
                if (C->P->nodes[c].type == TK_AST_BLOCK_ASSIGN ||
                    C->P->nodes[c].type == TK_AST_NONBLOCK) {
                    if (ndflt < 16) {
                        uint32_t lhs = C->P->nodes[c].first_child;
                        if (lhs && C->P->nodes[lhs].type == TK_AST_IDENT) {
                            dflts[ndflt].tgt = lw_fnet(C, lhs);
                            dflts[ndflt].rhs_n = lhs ?
                                C->P->nodes[lhs].next_sib : 0;
                            if (dflts[ndflt].tgt && dflts[ndflt].rhs_n)
                                ndflt++;
                        }
                    }
                } else {
                    /* Non-assign before case: just lower it */
                    lw_stms(C, c, is_reg);
                }
                c = C->P->nodes[c].next_sib;
            }

            /* Lower the case with pre-case defaults */
            lw_cmux_d(C, case_n, is_reg, dflts, ndflt);

            /* Lower post-case statements */
            c = C->P->nodes[case_n].next_sib;
            KA_GUARD(g2d, 10000);
            while (c && g2d--) {
                lw_stms(C, c, is_reg);
                c = C->P->nodes[c].next_sib;
            }
        } else {
            /* No case child: just iterate normally */
            c = C->P->nodes[nidx].first_child;
            KA_GUARD(g2e, 10000);
            while (c && g2e--) {
                lw_stms(C, c, is_reg);
                c = C->P->nodes[c].next_sib;
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ---- Lower one module ---- */

static void
lw_mod(lw_ctx_t *C, uint32_t mod_node)
{
    uint32_t c;

    c = C->P->nodes[mod_node].first_child;
    KA_GUARD(g, 100000);
    while (c && g--) {
        const tk_node_t *n = &C->P->nodes[c];

        if (n->type == TK_AST_COUNT) { c = n->next_sib; continue; }

        switch ((int)n->type) {
        case TK_AST_PORT:
        {
            /* Create port net */
            uint32_t name_n = 0, ch;
            uint8_t dir = 0;
            uint32_t w = lw_width(C, c);

            /* Determine direction from port text */
            const char *dtxt = C->P->lex->strs + n->d.text.off;
            if (n->d.text.len >= 5 && memcmp(dtxt, "input", 5) == 0) dir = 1;
            else if (n->d.text.len >= 6 && memcmp(dtxt, "output", 6) == 0) dir = 2;
            else if (n->d.text.len >= 5 && memcmp(dtxt, "inout", 5) == 0) dir = 3;

            /* Find IDENT child */
            ch = n->first_child;
            KA_GUARD(gp, 10);
            while (ch && gp--) {
                if (C->P->nodes[ch].type == TK_AST_IDENT) {
                    name_n = ch;
                    break;
                }
                ch = C->P->nodes[ch].next_sib;
            }

            if (name_n) {
                uint32_t ni = rt_anet(C->M,
                    lw_text(C, name_n), lw_tlen(C, name_n),
                    w, dir, C->radix);
                C->nmap[name_n] = ni;
            }
            break;
        }

        case TK_AST_NET_DECL:
        case TK_AST_VAR_DECL:
        {
            /* Check for memory (array) declaration first */
            uint32_t ch = n->first_child;
            uint32_t w = lw_width(C, c);
            uint32_t mmi = lw_mmem(C, c, w);
            if (mmi > 0) {
                /* Memory registered — skip net creation for it,
                 * reads/writes go through MEMRD/MEMWR cells */
                break;
            }
            /* Create internal net(s) */
            KA_GUARD(gd, 100);
            while (ch && gd--) {
                if (C->P->nodes[ch].type == TK_AST_IDENT) {
                    uint32_t ni = rt_anet(C->M,
                        lw_text(C, ch), lw_tlen(C, ch),
                        w, 0, C->radix);
                    C->nmap[ch] = ni;
                }
                ch = C->P->nodes[ch].next_sib;
            }
            break;
        }

        case TK_AST_ASSIGN:
        {
            /* Continuous assignment */
            lw_asgn(C, c, 0);
            break;
        }

        case TK_AST_ALWAYS_FF:
        {
            uint32_t ch = n->first_child;
            C->n_rdir = 0;
            KA_GUARD(ga, 100);
            while (ch && ga--) {
                if (C->P->nodes[ch].type != TK_AST_SENS_LIST)
                    lw_stms(C, ch, 1);
                ch = C->P->nodes[ch].next_sib;
            }
            /* Flush: rewire each chain's LAST cell to drive
             * the target net directly. No ASSIGN wrapper —
             * the cell's output just changes from tmp to tgt.
             * DFF inference sees MUX → tgt. Clean. */
            {
                uint32_t ri;
                for (ri = 0; ri < C->n_rdir; ri++) {
                    if (C->rdir[ri].cur != C->rdir[ri].net &&
                        C->rdir[ri].cell > 0 &&
                        C->rdir[ri].cell < C->M->n_cell) {
                        C->M->cells[C->rdir[ri].cell].out = C->rdir[ri].net;
                        C->M->nets[C->rdir[ri].net].driver = C->rdir[ri].cell;
                    }
                }
            }
            C->n_rdir = 0;
            break;
        }

        case TK_AST_ALWAYS_COMB:
        case TK_AST_ALWAYS:
        {
            uint32_t ch = n->first_child;
            C->n_rdir = 0;
            KA_GUARD(ga2, 100);
            while (ch && ga2--) {
                if (C->P->nodes[ch].type != TK_AST_SENS_LIST)
                    lw_stms(C, ch, 0);
                ch = C->P->nodes[ch].next_sib;
            }
            {
                uint32_t ri;
                for (ri = 0; ri < C->n_rdir; ri++) {
                    if (C->rdir[ri].cur != C->rdir[ri].net &&
                        C->rdir[ri].cell > 0 &&
                        C->rdir[ri].cell < C->M->n_cell) {
                        C->M->cells[C->rdir[ri].cell].out = C->rdir[ri].net;
                        C->M->nets[C->rdir[ri].net].driver = C->rdir[ri].cell;
                    }
                }
            }
            C->n_rdir = 0;
            break;
        }

        case TK_AST_INSTANCE:
        {
            /* Module instantiation: wire port connections.
             * The instance's op field holds (mod_index + 1)
             * as set by fl_annot. If 0, it's a black box. */
            uint32_t mi = (uint32_t)n->op;
            if (mi > 0) {
                /* Find the module definition node */
                uint32_t mod_def = 0;
                uint32_t mn = C->P->nodes[1].first_child;
                uint32_t mcount = 0;
                KA_GUARD(gm, 1000);
                while (mn && gm--) {
                    if (C->P->nodes[mn].type == TK_AST_MODULE) {
                        if (mcount == mi - 1) { mod_def = mn; break; }
                        mcount++;
                    }
                    mn = C->P->nodes[mn].next_sib;
                }

                if (mod_def) {
                    /* Get instance name from node text */
                    const char *inm = C->P->lex->strs + n->d.text.off;
                    uint16_t inl = n->d.text.len;

                    /* Lower the instantiated module body.
                     * Create prefixed nets for its ports/internals. */
                    char pfx[64];
                    uint32_t pfl;
                    uint32_t ich, pni;

                    /* Build prefix: "instname_" — but we use module
                     * name since instance node text is module name.
                     * The actual instance name is in a CONN child.
                     * For now, use the first IDENT child as instance name. */
                    ich = n->first_child;
                    KA_GUARD(gi2, 20);
                    while (ich && gi2--) {
                        if (C->P->nodes[ich].type == TK_AST_IDENT) break;
                        ich = C->P->nodes[ich].next_sib;
                    }
                    if (ich && C->P->nodes[ich].type == TK_AST_IDENT) {
                        pfl = C->P->nodes[ich].d.text.len;
                        if (pfl > 50) pfl = 50;
                        memcpy(pfx, C->P->lex->strs +
                               C->P->nodes[ich].d.text.off, pfl);
                        pfx[pfl] = '_';
                        pfl++;
                    } else {
                        memcpy(pfx, inm, inl > 50 ? 50 : inl);
                        pfl = inl > 50 ? 50 : inl;
                        pfx[pfl] = '_';
                        pfl++;
                    }
                    pfx[pfl] = '\0';

                    /* Create prefixed internal nets for the module */
                    {
                        uint32_t mc = C->P->nodes[mod_def].first_child;
                        KA_GUARD(gmc, 10000);
                        while (mc && gmc--) {
                            const tk_node_t *mn2 = &C->P->nodes[mc];
                            if (mn2->type == TK_AST_PORT ||
                                mn2->type == TK_AST_NET_DECL ||
                                mn2->type == TK_AST_VAR_DECL) {
                                uint32_t pch = mn2->first_child;
                                KA_GUARD(gpc, 100);
                                while (pch && gpc--) {
                                    if (C->P->nodes[pch].type == TK_AST_IDENT) {
                                        /* Create prefixed net */
                                        char pname[128];
                                        uint16_t pnl;
                                        uint32_t w2 = lw_width(C, mc);
                                        memcpy(pname, pfx, pfl);
                                        pnl = C->P->nodes[pch].d.text.len;
                                        if (pfl + pnl > 120) pnl = (uint16_t)(120 - pfl);
                                        memcpy(pname + pfl,
                                               C->P->lex->strs +
                                               C->P->nodes[pch].d.text.off,
                                               pnl);
                                        pname[pfl + pnl] = '\0';
                                        pni = rt_anet(C->M, pname,
                                               (uint16_t)(pfl + pnl),
                                               w2, 0, C->radix);
                                        C->nmap[pch] = pni;
                                    }
                                    pch = C->P->nodes[pch].next_sib;
                                }
                            }
                            mc = mn2->next_sib;
                        }
                    }

                    /* Lower the module body (assignments, always blocks) */
                    {
                        uint32_t mc = C->P->nodes[mod_def].first_child;
                        KA_GUARD(gmc2, 100000);
                        while (mc && gmc2--) {
                            const tk_node_t *mn2 = &C->P->nodes[mc];
                            if (mn2->type == TK_AST_ASSIGN)
                                lw_asgn(C, mc, 0);
                            else if (mn2->type == TK_AST_ALWAYS_FF) {
                                uint32_t ach = mn2->first_child;
                                KA_GUARD(gaf, 100);
                                while (ach && gaf--) {
                                    if (C->P->nodes[ach].type != TK_AST_SENS_LIST)
                                        lw_stms(C, ach, 1);
                                    ach = C->P->nodes[ach].next_sib;
                                }
                            } else if (mn2->type == TK_AST_ALWAYS_COMB ||
                                       mn2->type == TK_AST_ALWAYS) {
                                uint32_t ach = mn2->first_child;
                                KA_GUARD(gac, 100);
                                while (ach && gac--) {
                                    if (C->P->nodes[ach].type != TK_AST_SENS_LIST)
                                        lw_stms(C, ach, 0);
                                    ach = C->P->nodes[ach].next_sib;
                                }
                            }
                            mc = mn2->next_sib;
                        }
                    }

                    /* Wire port connections: each CONN child maps
                     * .port_name(expr) → ASSIGN between parent
                     * net and prefixed child net */
                    {
                        uint32_t cn = n->first_child;
                        KA_GUARD(gwire, 100);
                        while (cn && gwire--) {
                            if (C->P->nodes[cn].type == TK_AST_CONN) {
                                /* CONN: text=port_name, child=expr */
                                const char *prt = C->P->lex->strs +
                                    C->P->nodes[cn].d.text.off;
                                uint16_t prl = C->P->nodes[cn].d.text.len;
                                uint32_t expr = C->P->nodes[cn].first_child;

                                /* Find the prefixed port net */
                                char pname[128];
                                uint16_t ppnl;
                                uint32_t pnet = 0, enet, k2;

                                memcpy(pname, pfx, pfl);
                                ppnl = prl;
                                if (pfl + ppnl > 120) ppnl = (uint16_t)(120 - pfl);
                                memcpy(pname + pfl, prt, ppnl);
                                pname[pfl + ppnl] = '\0';

                                /* Find the prefixed net */
                                for (k2 = C->net_lo ? C->net_lo : 1; k2 < C->M->n_net; k2++) {
                                    const rt_net_t *nn = &C->M->nets[k2];
                                    if (nn->name_len == (uint16_t)(pfl + ppnl) &&
                                        memcmp(C->M->strs + nn->name_off,
                                               pname, pfl + ppnl) == 0) {
                                        pnet = k2;
                                        break;
                                    }
                                }

                                if (pnet && expr) {
                                    enet = lw_expr(C, expr);
                                    if (enet) {
                                        /* Wire: ASSIGN from parent to child
                                         * or child to parent based on port dir.
                                         * For simplicity: input ports get ASSIGN
                                         * from parent, output ports get ASSIGN
                                         * to parent. Use bidirectional ASSIGN. */
                                        uint32_t ww = C->M->nets[pnet].width;
                                        uint32_t ins2[1];
                                        ins2[0] = enet;
                                        rt_acell(C->M, RT_ASSIGN, pnet, ins2, 1, ww);
                                        ins2[0] = pnet;
                                        rt_acell(C->M, RT_ASSIGN, enet, ins2, 1, ww);
                                    }
                                }
                            }
                            cn = C->P->nodes[cn].next_sib;
                        }
                    }
                }
            }
            break;
        }

        default:
            break;
        }

        c = n->next_sib;
    }
}

/* ---- DFF inference ----
 * After lowering, every net marked is_reg needs a DFF.
 * The MUX chain feeding it becomes the D input.
 * Clock comes from the sensitivity list (posedge clk).
 * Reset comes from async edges (negedge rst_n).
 *
 * Like fitting a latch to the barn door: the logic cone
 * already exists, we just need to say "hold this on the
 * clock edge, please." */

static void
lw_dffs(lw_ctx_t *C, uint32_t mod_n, uint32_t net_lo)
{
    uint32_t i;
    uint32_t clk = 0, rst = 0;
    /* net_lo: first net index created for this module.
     * Only process nets in [net_lo, n_net). */

    /* Find clock and reset from sensitivity lists.
     * Walk always_ff blocks, extract posedge/negedge idents. */
    {
        uint32_t ch = C->P->nodes[mod_n].first_child;
        KA_GUARD(gs, 10000);
        while (ch && gs--) {
            if (C->P->nodes[ch].type == TK_AST_ALWAYS_FF) {
                uint32_t sl = C->P->nodes[ch].first_child;
                KA_GUARD(gs2, 20);
                while (sl && gs2--) {
                    if (C->P->nodes[sl].type == TK_AST_SENS_LIST) {
                        uint32_t se = C->P->nodes[sl].first_child;
                        KA_GUARD(gs3, 20);
                        while (se && gs3--) {
                            if (C->P->nodes[se].type == TK_AST_SENS_EDGE) {
                                /* Edge type in d.text, ident is child */
                                const char *et = C->P->lex->strs +
                                    C->P->nodes[se].d.text.off;
                                uint32_t id = C->P->nodes[se].first_child;
                                if (id && C->P->nodes[id].type == TK_AST_IDENT) {
                                    uint32_t ni = lw_fnet(C, id);
                                    if (C->P->nodes[se].d.text.len >= 7 &&
                                        memcmp(et, "posedge", 7) == 0) {
                                        if (clk == 0) clk = ni;
                                    } else if (C->P->nodes[se].d.text.len >= 7 &&
                                               memcmp(et, "negedge", 7) == 0) {
                                        if (rst == 0) rst = ni;
                                    }
                                }
                            }
                            se = C->P->nodes[se].next_sib;
                        }
                    }
                    sl = C->P->nodes[sl].next_sib;
                }
            }
            ch = C->P->nodes[ch].next_sib;
        }
    }

    /* For each registered net, insert a DFF cell.
     * The current driver(s) become the D input.
     * DFF output drives the net. */
    for (i = net_lo; i < C->M->n_net; i++) {
        rt_net_t *net = &C->M->nets[i];
        uint32_t drv, din, ins[3], j;
        rt_ctype_t dtyp;

        if (!net->is_reg) continue;
        drv = net->driver;
        if (drv == 0 || drv >= C->M->n_cell) continue;

        /* Create a net for the D input (pre-DFF logic output).
         * Rewire ALL cells driving this net to feed din. */
        din = rt_anet(C->M, "dff_d", 5, net->width, 0, C->radix);

        for (j = 1; j < C->M->n_cell; j++) {
            if (C->M->cells[j].type == RT_CELL_COUNT) continue;
            if (C->M->cells[j].out == i) {
                C->M->cells[j].out = din;
                C->M->nets[din].driver = j;
            }
        }

        /* Also rewrite any cell input that referenced this
         * net as a feedback path — those should read the
         * DFF output (the net itself), not din. So we leave
         * inputs alone; only outputs get redirected. */

        /* Create DFF cell: ins[0]=D, ins[1]=CLK, [ins[2]=RST] */
        if (rst != 0) {
            dtyp = RT_DFFR;
            ins[0] = din; ins[1] = clk; ins[2] = rst;
            rt_acell(C->M, dtyp, i, ins, 3, net->width);
        } else {
            dtyp = RT_DFF;
            ins[0] = din; ins[1] = clk;
            rt_acell(C->M, dtyp, i, ins, 2, net->width);
        }
    }
}

/* ---- Internal: build RTL module from AST ---- */

static rt_mod_t *
lw_core(const tk_parse_t *P, const ce_val_t *cv,
        const wi_val_t *wv, uint32_t nvals, uint8_t radix)
{
    lw_ctx_t C;
    rt_mod_t *M;
    uint32_t c;

    if (!P || !cv || !wv) return NULL;

    M = (rt_mod_t *)calloc(1, sizeof(rt_mod_t));
    if (!M) return NULL;

    if (rt_init(M, TK_MAX_NETS, TK_MAX_CELLS) != 0) {
        free(M);
        return NULL;
    }

    memset(&C, 0, sizeof(C));
    C.P     = P;
    C.cv    = cv;
    C.wv    = wv;
    C.nvals = nvals;
    C.M     = M;
    C.radix = radix;

    /* Net mapping table */
    C.nmap = (uint32_t *)calloc(P->n_node, sizeof(uint32_t));
    C.nmsz = P->n_node;
    if (!C.nmap) { rt_free(M); free(M); return NULL; }

    /* Find the top-level module: the one not instantiated by
     * any other module. Sub-modules are inlined at their
     * INSTANCE sites — lowering them standalone would create
     * duplicate un-prefixed nets (the bug that gave us three
     * copies of 'y' all fighting over n2). */
    {
        /* Collect module nodes */
        uint32_t mods[256];
        uint8_t  used[256]; /* 1 = instantiated by another module */
        int nmod = 0;

        c = P->nodes[1].first_child;
        KA_GUARD(gm1, 1000);
        while (c && gm1-- && nmod < 256) {
            if (P->nodes[c].type == TK_AST_MODULE) {
                mods[nmod] = c;
                used[nmod] = 0;
                nmod++;
            }
            c = P->nodes[c].next_sib;
        }

        /* Mark modules that are targets of INSTANCE nodes.
         * Walk the entire AST — instances can be nested inside
         * generate-for blocks, begin/end, etc. */
        {
            uint32_t ni;
            for (ni = 1; ni < P->n_node; ni++) {
                if (P->nodes[ni].type == TK_AST_INSTANCE) {
                    uint32_t tgt = (uint32_t)P->nodes[ni].op;
                    if (tgt > 0 && (int)(tgt - 1) < nmod)
                        used[tgt - 1] = 1;
                }
            }
        }

        /* Lower all un-instantiated (top-level) modules.
         * Each module gets its own net scope so names like
         * 'sum' in one module don't collide with 'sum' in
         * another. The nmap reset + net_lo scoping together
         * prevent cross-module name collisions. */
        {
            int mi;
            for (mi = 0; mi < nmod; mi++) {
                if (!used[mi]) {
                    uint32_t nlo = M->n_net;
                    C.net_lo = nlo;
                    {
                        const char *mn = P->lex->strs +
                            P->nodes[mods[mi]].d.text.off;
                        uint16_t ml = P->nodes[mods[mi]].d.text.len;
                        if (ml > 63) ml = 63;
                        memcpy(M->mod_name, mn, ml);
                        M->mod_name[ml] = '\0';
                    }
                    printf("takahe: lowering module '%s'\n",
                           M->mod_name);
                    lw_mod(&C, mods[mi]);

                    /* ---- Chain multi-driver MUX nets ----
                     * Sequential bare ifs (if(c1) x=1; if(c2) x=2;)
                     * each create a MUX driving x. Chain them:
                     * all but the last redirect to intermediate nets,
                     * each subsequent MUX uses the previous as hold. */
                    {
                        uint32_t ni;
                        for (ni = nlo; ni < M->n_net; ni++) {
                            uint32_t drvs[32];
                            int nd = 0;
                            uint32_t ci;
                            /* Collect all cells driving this net */
                            for (ci = 1; ci < M->n_cell && nd < 32; ci++) {
                                if (M->cells[ci].type == RT_CELL_COUNT) continue;
                                if (M->cells[ci].out == ni)
                                    drvs[nd++] = ci;
                            }
                            if (nd <= 1) continue;
                            /* Chain: redirect all but last to tmp nets.
                             * Each MUX's d0 (hold) input becomes the
                             * previous MUX's output. */
                            {
                                int di;
                                uint32_t w = M->nets[ni].width;
                                /* Keep LAST driver on the target net.
                                 * Redirect all earlier drivers to tmps.
                                 * For MUX chains: patch hold inputs so
                                 * each MUX reads the previous output.
                                 * For ASSIGNs: just redirect to dead net
                                 * (DCE will clean them up). */
                                for (di = 0; di < nd - 1; di++) {
                                    uint32_t tmp = rt_anet(M, "chn", 3,
                                        w, 0, C.radix);
                                    M->cells[drvs[di]].out = tmp;
                                    /* Patch next driver's inputs: if it
                                     * reads the target net as hold/feedback,
                                     * update to read our output instead. */
                                    /* Patch only co-drivers: cells that
                                     * also drive this net and read it as
                                     * hold/feedback. Safe because these are
                                     * in the same sequential block. */
                                    {
                                        int nxt;
                                        for (nxt = di + 1; nxt < nd; nxt++) {
                                            uint8_t pi;
                                            rt_cell_t *nc = &M->cells[drvs[nxt]];
                                            for (pi = 0; pi < nc->n_in; pi++) {
                                                if (nc->ins[pi] == ni)
                                                    nc->ins[pi] = tmp;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    lw_dffs(&C, mods[mi], nlo);
                    memset(C.nmap, 0, P->n_node * sizeof(uint32_t));
                }
            }
        }
    }

    printf("takahe: RTL: %u nets, %u cells\n",
           M->n_net - 1, M->n_cell - 1);

    free(C.nmap);
    return M;
}

/* ---- Public: build and return RTL module ---- */

rt_mod_t *
lw_build(const tk_parse_t *P, const ce_val_t *cv,
         const wi_val_t *wv, uint32_t nvals)
{
    return lw_core(P, cv, wv, nvals, TK_RADIX_BIN);
}

rt_mod_t *
lw_build_r(const tk_parse_t *P, const ce_val_t *cv,
           const wi_val_t *wv, uint32_t nvals, uint8_t radix)
{
    return lw_core(P, cv, wv, nvals, radix);
}

/* ---- Public: lower, dump, free (original API) ---- */

int
lw_lower(const tk_parse_t *P, const ce_val_t *cv,
         const wi_val_t *wv, uint32_t nvals)
{
    rt_mod_t *M = lw_core(P, cv, wv, nvals, TK_RADIX_BIN);
    if (!M) return -1;

    rt_dump(M);
    rt_free(M);
    free(M);
    return 0;
}
