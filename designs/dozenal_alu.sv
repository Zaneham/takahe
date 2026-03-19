// dozenal_alu.sv -- Duodecimal ALU: Base-12 Arithmetic in Silicon
//
// The Mesopotamians were right. The Dozenal Society has been
// saying this since 1944. Now they have hardware.
//
// 12 is divisible by 2, 3, 4, and 6. 10 is divisible by 2 and 5.
// One-third in decimal: 0.333333... (infinite, inexact).
// One-third in dozenal: 0.4 (exact, one digit).
// This ALU does the maths that base-10 can't.
//
// Architecture:
//   - 4-digit duodecimal data path (12^4 = 20,736 values)
//   - Each digit: 4 binary bits (encoding 0-11)
//   - Operations: ADD, SUB, AND (min), OR (max), NOT (complement)
//   - Ripple-carry dozenal adder (carry at 12, not 2 or 10)
//
// Applications nobody has built hardware for until now:
//   - Clock arithmetic (12 hours, natively)
//   - Musical pitch (12 semitones, natively)
//   - Angle subdivision (360 = 30 × 12)
//   - Imperial measurement (12 inches, 12 pence)
//   - Merchant counting (dozens, grosses, great grosses)
//
// Ea-nasir would have sold you this chip with wrong dopant levels.

// ---- Single dozenal digit operations (4-bit encoded, values 0-11) ----

// Dozenal NOT: complement = 11 - digit
module doz_not (
    input  logic [3:0] a,
    output logic [3:0] y
);
    assign y = 4'd11 - a;
endmodule

// Dozenal AND: min(a, b)
module doz_and (
    input  logic [3:0] a,
    input  logic [3:0] b,
    output logic [3:0] y
);
    assign y = (a < b) ? a : b;
endmodule

// Dozenal OR: max(a, b)
module doz_or (
    input  logic [3:0] a,
    input  logic [3:0] b,
    output logic [3:0] y
);
    assign y = (a > b) ? a : b;
endmodule

// Dozenal HALF ADDER: a + b with carry at 12
// 7 sheep + 8 sheep = 3 sheep carry 1 dozen.
// A Sumerian merchant would understand this immediately.
module doz_hadd (
    input  logic [3:0] a,
    input  logic [3:0] b,
    output logic [3:0] sum,
    output logic       carry
);
    logic [4:0] total;
    assign total = {1'b0, a} + {1'b0, b};
    assign carry = (total >= 5'd12);
    assign sum   = carry ? (total[3:0] - 4'd12) : total[3:0];
endmodule

// Dozenal FULL ADDER: a + b + cin with carry at 12
module doz_fadd (
    input  logic [3:0] a,
    input  logic [3:0] b,
    input  logic       cin,
    output logic [3:0] sum,
    output logic       cout
);
    logic [4:0] total;
    assign total = {1'b0, a} + {1'b0, b} + {4'b0, cin};
    assign cout  = (total >= 5'd12);
    assign sum   = cout ? (total[3:0] - 4'd12) : total[3:0];
endmodule

// ---- 4-digit Dozenal ALU ----
// 16 binary bits internally, 4 dozenal digits.
// Range: 0 to 20,735 (0000 to BBBB in dozenal).
// Or signed: -10,368 to +10,367 using top digit as sign.

module dozenal_alu (
    input  logic [15:0] a,       // 4 digits × 4 bits
    input  logic [15:0] b,
    input  logic [2:0]  op,      // 000=ADD 001=SUB 010=AND 011=OR 100=NOT
    output logic [15:0] result,
    output logic        carry_out,
    output logic        zero
);
    // Per-digit operations
    logic [3:0] d_and[4], d_or[4], d_not[4];
    logic [3:0] d_sum[4];
    logic       d_carry[5];

    genvar i;

    // AND (min) per digit
    generate
        for (i = 0; i < 4; i = i + 1) begin : gen_and
            doz_and u_and (
                .a(a[i*4+3:i*4]),
                .b(b[i*4+3:i*4]),
                .y(d_and[i])
            );
        end
    endgenerate

    // OR (max) per digit
    generate
        for (i = 0; i < 4; i = i + 1) begin : gen_or
            doz_or u_or (
                .a(a[i*4+3:i*4]),
                .b(b[i*4+3:i*4]),
                .y(d_or[i])
            );
        end
    endgenerate

    // NOT (complement) per digit
    generate
        for (i = 0; i < 4; i = i + 1) begin : gen_not
            doz_not u_not (
                .a(a[i*4+3:i*4]),
                .y(d_not[i])
            );
        end
    endgenerate

    // Ripple-carry dozenal adder
    // Carry happens at 12, not 2 or 10.
    // The Sumerians had this right 5,000 years ago.
    assign d_carry[0] = (op == 3'b001) ? 1'b1 : 1'b0; // SUB: cin=1

    generate
        for (i = 0; i < 4; i = i + 1) begin : gen_add
            doz_fadd u_fa (
                .a(a[i*4+3:i*4]),
                .b((op == 3'b001) ? (4'd11 - b[i*4+3:i*4]) : b[i*4+3:i*4]),
                .cin(d_carry[i]),
                .sum(d_sum[i]),
                .cout(d_carry[i+1])
            );
        end
    endgenerate

    // Result mux
    always_comb begin
        carry_out = 1'b0;
        case (op)
            3'b000: begin // ADD
                result = {d_sum[3], d_sum[2], d_sum[1], d_sum[0]};
                carry_out = d_carry[4];
            end
            3'b001: begin // SUB (12's complement: NOT + 1)
                result = {d_sum[3], d_sum[2], d_sum[1], d_sum[0]};
                carry_out = d_carry[4];
            end
            3'b010: begin // AND (min)
                result = {d_and[3], d_and[2], d_and[1], d_and[0]};
            end
            3'b011: begin // OR (max)
                result = {d_or[3], d_or[2], d_or[1], d_or[0]};
            end
            3'b100: begin // NOT (complement: 11-digit per position)
                result = {d_not[3], d_not[2], d_not[1], d_not[0]};
            end
            default: begin
                result = a;
            end
        endcase

        zero = (result == 16'h0);
    end
endmodule
