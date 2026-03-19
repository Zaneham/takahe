// setun70.sv -- Setun-70 Ternary Processor
//
// A hardware implementation of Brusentsov's 1970 ternary computer.
// Reconstructed from the emulator at /c/dev/emulators/setun70-emulator.
//
// Architecture (from the original specification):
//   - Balanced ternary (trits: -1, 0, +1)
//   - 6-trit syllables (trytes), range -364 to +364
//   - Two-stack POLIZ (Reverse Polish) execution
//   - 27 pages × 81 syllables memory
//   - 3 page registers for bank switching
//
// This is a simplified core: ALU + stack + decoder.
// Full memory subsystem would be a separate module.
//
// With --radix 3, every data signal is ternary.
// Control logic (state machine, decoder) stays binary
// because opcodes are encoded in binary even on the
// original Setun. The DATA PATH is ternary. The CONTROL
// is binary. Mixed-radix, as Brusentsov intended.
//
// Nikolai Petrovich Brusentsov, 1925-2014.
// He built the only production ternary computer.
// The world ignored it. We're un-ignoring it.

// ---- ALU: ternary arithmetic logic unit ----

module setun_alu (
    input  logic [5:0] a,
    input  logic [5:0] b,
    input  logic [2:0] op,
    output logic [5:0] result,
    output logic [1:0] flag     // -1, 0, +1 (comparison result)
);
    // opcodes
    // 000 = ADD    001 = SUB    010 = AND (min)
    // 011 = OR (max)  100 = NOT (neg)  101 = CMP
    // 110 = PASS A    111 = ZERO

    always_comb begin
        flag = 2'b00;
        case (op)
            3'b000: result = a + b;
            3'b001: result = a - b;
            3'b010: result = a & b;
            3'b011: result = a | b;
            3'b100: result = ~a;        // FREE in balanced ternary
            3'b101: begin               // CMP
                if (a == b)      begin result = '0; flag = 2'b00; end
                else if (a == b) begin result = '0; flag = 2'b01; end // placeholder
                else             begin result = '0; flag = 2'b10; end
            end
            3'b110: result = a;
            default: result = '0;
        endcase
    end
endmodule

// ---- Stack: 16-deep operand stack ----
// POLIZ needs a stack. The Setun-70 spec doesn't say
// how deep, but 16 trytes is enough for any reasonable
// expression and fits nicely in a small register file.

module setun_stk (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       push,
    input  logic       pop,
    input  logic [5:0] din,
    output logic [5:0] top,     // T (top of stack)
    output logic [5:0] sec      // S (second element)
);
    // 16-deep stack as a shift register
    logic [5:0] stk [0:15];
    logic [3:0] sp;

    assign top = stk[sp];
    assign sec = (sp > 0) ? stk[sp - 1] : '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sp <= '0;
        end else if (push) begin
            sp <= sp + 1;
            stk[sp + 1] <= din;
        end else if (pop) begin
            sp <= sp - 1;
        end
    end
endmodule

// ---- Decoder: instruction decoder ----
// Syllable format:
//   [0:1] == 00 → operation syllable
//     [2]     → type: 0=basic, 1=service, -1=macro
//     [3:5]   → opcode (-13 to +13)
//   [0:1] != 00 → address syllable
//     [0]     → word length (-1, 0, 1)
//     [1]     → page register (-1, 0, 1)
//     [2:5]   → offset (-40 to +40)

module setun_dec (
    input  logic [5:0] syllable,
    output logic       is_op,     // 1 = operation, 0 = address
    output logic [2:0] alu_op,    // ALU operation code
    output logic       do_push,   // push result to stack
    output logic       do_pop,    // pop from stack
    output logic       do_halt    // halt execution
);
    logic [1:0] prefix;
    logic [2:0] opcode;

    assign prefix = syllable[5:4];  // top 2 trits
    assign opcode = syllable[2:0];  // bottom 3 trits
    assign is_op  = (prefix == 2'b00);

    always_comb begin
        alu_op  = 3'b110;  // default: PASS
        do_push = 1'b0;
        do_pop  = 1'b0;
        do_halt = 1'b0;

        if (is_op) begin
            case (opcode)
                3'b001: begin alu_op = 3'b000; do_pop = 1'b1; do_push = 1'b1; end // ADD
                3'b010: begin alu_op = 3'b001; do_pop = 1'b1; do_push = 1'b1; end // SUB
                3'b101: begin alu_op = 3'b101; do_pop = 1'b1; do_push = 1'b1; end // CMP
                3'b110: begin do_pop = 1'b1; end                                    // DROP
                3'b111: begin do_push = 1'b1; end                                   // DUP
                default: do_halt = 1'b1;                                            // HALT/unknown
            endcase
        end
    end
endmodule

// ---- Top: Setun-70 processor core ----

module setun70 (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [5:0] instr,    // instruction syllable input
    input  logic       valid,    // instruction valid
    output logic [5:0] tos,      // top of stack (visible output)
    output logic [1:0] flags,    // comparison flags
    output logic       halted    // processor halted
);
    logic       is_op, do_push, do_pop, do_halt;
    logic [2:0] alu_op;
    logic [5:0] alu_a, alu_b, alu_result;
    logic [1:0] alu_flag;
    logic [5:0] stk_top, stk_sec;

    // Decoder
    setun_dec dec (
        .syllable(instr),
        .is_op(is_op),
        .alu_op(alu_op),
        .do_push(do_push),
        .do_pop(do_pop),
        .do_halt(do_halt)
    );

    // ALU — ternary data path
    setun_alu alu (
        .a(stk_top),
        .b(stk_sec),
        .op(alu_op),
        .result(alu_result),
        .flag(alu_flag)
    );

    // Stack
    setun_stk stack (
        .clk(clk),
        .rst_n(rst_n),
        .push(valid & do_push),
        .pop(valid & do_pop),
        .din(alu_result),
        .top(stk_top),
        .sec(stk_sec)
    );

    assign tos    = stk_top;
    assign flags  = alu_flag;

    // Halt register
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            halted <= 1'b0;
        else if (valid & do_halt)
            halted <= 1'b1;
    end
endmodule
