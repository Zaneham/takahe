// setun_alu.sv -- Balanced Ternary ALU (6-trit, 1 tryte)
//
// The Setun's revenge. 68 years after Brusentsov built the
// world's only production ternary computer, someone finally
// has a tool that can synthesise one.
//
// Architecture: 6-trit data path (1 tryte = 729 values)
// Range: -364 to +364 in balanced ternary
// Operations: ADD, AND (min), OR (max), NOT (neg), PASS
//
// With --radix 3, every signal is a trit {-1, 0, +1}.
// width=6 means 6 trits, not 6 bits.
// AND = min(a,b). OR = max(a,b). NOT = -a.
// Negation is FREE: no carry chain, no complement.
// That's the whole point of balanced ternary.

module setun_alu (
    input  logic [5:0] a,        // operand A (6 trits)
    input  logic [5:0] b,        // operand B (6 trits)
    input  logic [2:0] op,       // operation select (binary control)
    output logic [5:0] result,   // ALU output (6 trits)
    output logic       zero      // result == 0 flag
);

    // Operation codes (binary — control logic stays binary
    // even when the data path is ternary, because that's how
    // real mixed-radix designs work)
    //
    // 000 = ADD   (ternary addition)
    // 001 = AND   (min, trit-wise)
    // 010 = OR    (max, trit-wise)
    // 011 = NOT   (negate A — FREE in balanced ternary!)
    // 100 = PASS  (pass A through)

    always_comb begin
        case (op)
            3'b000:  result = a + b;      // ternary ADD
            3'b001:  result = a & b;      // ternary AND (min)
            3'b010:  result = a | b;      // ternary OR (max)
            3'b011:  result = ~a;         // ternary NOT (neg)
            3'b100:  result = a;          // PASS
            default: result = '0;         // zero
        endcase

        zero = (result == '0);
    end

endmodule
