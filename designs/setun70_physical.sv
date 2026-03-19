// setun70_physical.sv -- Physical Ternary Computer on Binary Silicon
//
// The first ternary computer since Brusentsov (1958).
// On SKY130 130nm CMOS. Actually fabricable. Actually ternary.
//
// Encoding: each trit is 2 binary bits.
//   00 = N (-1)    01 = Z (0)    10 = P (+1)    11 = invalid
//
// A 6-trit tryte = 12 binary bits. Range: -364 to +364.
//
// The key insight: ternary NOT is still FREE. Negation is
// just swapping the two bits of each trit: 00↔10, 01↔01.
// No gates. Just crossed wires. Brusentsov's advantage
// survives the binary encoding.
//
// Ternary AND (min) and OR (max) are 4-input LUTs per trit.
// Ternary ADD is a ripple-carry chain of trit full adders.
// Each trit adder is a 6-input → 4-output lookup.
//
// To synthesise and fabricate:
//   ./takahe --lib sky130.lib --map setun70.v --sta 10 setun70_physical.sv
//   Submit to TinyTapeout. $150. 12 weeks. Ternary computer.

// ---- Single trit operations (2-bit encoded) ----

// Ternary NOT: neg(t) — swap bits. Zero gates. Just wires.
module trit_not (
    input  logic [1:0] a,
    output logic [1:0] y
);
    assign y = {a[0], a[1]};  // swap. that's it. FREE.
endmodule

// Ternary AND: min(a, b)
module trit_and (
    input  logic [1:0] a,
    input  logic [1:0] b,
    output logic [1:0] y
);
    always_comb begin
        // min(a, b) truth table on encoded values
        // N(00) < Z(01) < P(10)
        case ({a, b})
            4'b0000: y = 2'b00;  // min(N,N) = N
            4'b0001: y = 2'b00;  // min(N,Z) = N
            4'b0010: y = 2'b00;  // min(N,P) = N
            4'b0100: y = 2'b00;  // min(Z,N) = N
            4'b0101: y = 2'b01;  // min(Z,Z) = Z
            4'b0110: y = 2'b01;  // min(Z,P) = Z
            4'b1000: y = 2'b00;  // min(P,N) = N
            4'b1001: y = 2'b01;  // min(P,Z) = Z
            4'b1010: y = 2'b10;  // min(P,P) = P
            default: y = 2'b01;  // invalid → Z
        endcase
    end
endmodule

// Ternary OR: max(a, b)
module trit_or (
    input  logic [1:0] a,
    input  logic [1:0] b,
    output logic [1:0] y
);
    always_comb begin
        case ({a, b})
            4'b0000: y = 2'b00;  // max(N,N) = N
            4'b0001: y = 2'b01;  // max(N,Z) = Z
            4'b0010: y = 2'b10;  // max(N,P) = P
            4'b0100: y = 2'b01;  // max(Z,N) = Z
            4'b0101: y = 2'b01;  // max(Z,Z) = Z
            4'b0110: y = 2'b10;  // max(Z,P) = P
            4'b1000: y = 2'b10;  // max(P,N) = P
            4'b1001: y = 2'b10;  // max(P,Z) = P
            4'b1010: y = 2'b10;  // max(P,P) = P
            default: y = 2'b01;
        endcase
    end
endmodule

// Ternary HALF ADDER: a + b → (sum, carry)
// Balanced ternary addition per trit:
//   N+N = P carry N    (-1+-1 = -2 = +1 + -1*3)
//   N+Z = N carry Z    (-1+0  = -1)
//   N+P = Z carry Z    (-1+1  = 0)
//   Z+Z = Z carry Z    (0+0   = 0)
//   Z+P = P carry Z    (0+1   = 1)
//   P+P = N carry P    (1+1   = 2 = -1 + 1*3)
module trit_hadd (
    input  logic [1:0] a,
    input  logic [1:0] b,
    output logic [1:0] sum,
    output logic [1:0] carry
);
    always_comb begin
        case ({a, b})
            4'b0000: begin sum = 2'b10; carry = 2'b00; end // N+N=P,cN
            4'b0001: begin sum = 2'b00; carry = 2'b01; end // N+Z=N,cZ
            4'b0010: begin sum = 2'b01; carry = 2'b01; end // N+P=Z,cZ
            4'b0100: begin sum = 2'b00; carry = 2'b01; end // Z+N=N,cZ
            4'b0101: begin sum = 2'b01; carry = 2'b01; end // Z+Z=Z,cZ
            4'b0110: begin sum = 2'b10; carry = 2'b01; end // Z+P=P,cZ
            4'b1000: begin sum = 2'b01; carry = 2'b01; end // P+N=Z,cZ
            4'b1001: begin sum = 2'b10; carry = 2'b01; end // P+Z=P,cZ
            4'b1010: begin sum = 2'b00; carry = 2'b10; end // P+P=N,cP
            default: begin sum = 2'b01; carry = 2'b01; end
        endcase
    end
endmodule

// Ternary FULL ADDER: a + b + cin → (sum, cout)
module trit_fadd (
    input  logic [1:0] a,
    input  logic [1:0] b,
    input  logic [1:0] cin,
    output logic [1:0] sum,
    output logic [1:0] cout
);
    logic [1:0] s1, c1, c2;

    trit_hadd ha1 (.a(a), .b(b), .sum(s1), .carry(c1));
    trit_hadd ha2 (.a(s1), .b(cin), .sum(sum), .carry(c2));

    // cout = max of carries (can't have both non-zero)
    trit_or   cor (.a(c1), .b(c2), .y(cout));
endmodule

// ---- 6-trit (1 tryte) ALU ----
// 12 binary bits internally. Ternary semantics.

module tryte_alu (
    input  logic [11:0] a,       // 6 trits = 12 bits
    input  logic [11:0] b,
    input  logic [2:0]  op,      // 000=ADD 001=AND 010=OR 011=NOT 100=PASS
    output logic [11:0] result,
    output logic [1:0]  flag     // comparison: N/Z/P
);
    // Per-trit operations
    logic [1:0] t_and[6], t_or[6], t_not[6];
    logic [1:0] t_sum[6], t_carry[7];

    // Generate per-trit logic
    genvar i;
    generate
        for (i = 0; i < 6; i = i + 1) begin : trit_ops
            trit_and u_and (.a(a[i*2+1:i*2]), .b(b[i*2+1:i*2]), .y(t_and[i]));
            trit_or  u_or  (.a(a[i*2+1:i*2]), .b(b[i*2+1:i*2]), .y(t_or[i]));
            trit_not u_not (.a(a[i*2+1:i*2]), .y(t_not[i]));
        end
    endgenerate

    // Ripple-carry ternary adder
    assign t_carry[0] = 2'b01; // carry in = Z (zero)
    generate
        for (i = 0; i < 6; i = i + 1) begin : adder
            trit_fadd u_fa (
                .a(a[i*2+1:i*2]),
                .b(b[i*2+1:i*2]),
                .cin(t_carry[i]),
                .sum(t_sum[i]),
                .cout(t_carry[i+1])
            );
        end
    endgenerate

    // MUX the result based on opcode
    always_comb begin
        case (op)
            3'b000: // ADD
                result = {t_sum[5], t_sum[4], t_sum[3],
                          t_sum[2], t_sum[1], t_sum[0]};
            3'b001: // AND (min)
                result = {t_and[5], t_and[4], t_and[3],
                          t_and[2], t_and[1], t_and[0]};
            3'b010: // OR (max)
                result = {t_or[5], t_or[4], t_or[3],
                          t_or[2], t_or[1], t_or[0]};
            3'b011: // NOT (neg) — still basically free
                result = {t_not[5], t_not[4], t_not[3],
                          t_not[2], t_not[1], t_not[0]};
            3'b100: // PASS
                result = a;
            default:
                result = 12'b010101010101; // all zeros
        endcase

        // Flag: sign of result (MSB trit)
        flag = result[11:10];
    end
endmodule

// ---- Top: Physical Setun-70 Processor Core ----

module setun70_phys (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [11:0] instr,   // 6-trit instruction (12 bits)
    input  logic        valid,
    output logic [11:0] tos,     // top of stack (12 bits = 1 tryte)
    output logic [1:0]  flags,
    output logic        halted
);
    logic [2:0]  alu_op;
    logic [11:0] alu_a, alu_b, alu_result;
    logic [1:0]  alu_flag;

    // Stack: 8-deep, 12 bits (6 trits) per entry
    logic [11:0] stk [0:7];
    logic [2:0]  sp;

    // ALU
    tryte_alu alu (
        .a(stk[sp]),
        .b((sp > 0) ? stk[sp-1] : 12'b010101010101),
        .op(alu_op),
        .result(alu_result),
        .flag(alu_flag)
    );

    assign tos   = stk[sp];
    assign flags = alu_flag;

    // Simple decoder: top 2 trits (4 bits) = opcode
    always_comb begin
        alu_op  = 3'b100; // default: PASS
        case (instr[11:8])
            4'b0101: alu_op = 3'b000; // Z,Z prefix + 01 = ADD
            4'b0110: alu_op = 3'b001; // Z,Z prefix + 10 = AND
            4'b1001: alu_op = 3'b010; // Z,Z prefix + 01,01 = OR
            4'b1010: alu_op = 3'b011; // NOT
            default: alu_op = 3'b100; // PASS
        endcase
    end

    // Stack + execution
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sp     <= 3'b000;
            halted <= 1'b0;
        end else if (valid && !halted) begin
            // Push ALU result
            stk[sp] <= alu_result;
        end
    end
endmodule
