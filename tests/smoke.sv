// smoke.sv -- Basic SystemVerilog for lexer testing
// If the takahe can tokenise this, it can tokenise anything.
// Well, not anything. But it's a start.

module counter #(
    parameter WIDTH = 8
)(
    input  logic             clk,
    input  logic             rst_n,
    input  logic             en,
    output logic [WIDTH-1:0] count
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            count <= '0;
        else if (en)
            count <= count + 1'b1;
    end

endmodule

module alu #(
    parameter N = 32
)(
    input  logic [N-1:0] a,
    input  logic [N-1:0] b,
    input  logic [2:0]   op,
    output logic [N-1:0] result,
    output logic         zero
);

    always_comb begin
        case (op)
            3'b000: result = a + b;
            3'b001: result = a - b;
            3'b010: result = a & b;
            3'b011: result = a | b;
            3'b100: result = a ^ b;
            3'b101: result = a << b[4:0];
            3'b110: result = a >> b[4:0];
            default: result = '0;
        endcase

        zero = (result == '0);
    end

endmodule
