module addsub8(input logic sub,
               input logic [7:0] a,
               input logic [7:0] b,
               output logic [7:0] y,
               output logic cout);
    logic [8:0] t;
    assign t = sub ? ({1'b0, a} - {1'b0, b}) : ({1'b0, a} + {1'b0, b});
    assign y = t[7:0];
    assign cout = t[8];
endmodule

module reg8(input logic clk,
            input logic rst_n,
            input logic en,
            input logic [7:0] d,
            output logic [7:0] q);
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)   q <= 8'd0;
        else if (en)  q <= d;
    end
endmodule

module mux8(input logic s,
            input logic [7:0] a,
            input logic [7:0] b,
            output logic [7:0] y);
    assign y = s ? b : a;
endmodule

module datapath(input logic clk,
                input logic rst_n,
                input logic sub,
                input logic ld,
                input logic selb,
                input logic [7:0] imm,
                input logic [7:0] din,
                output logic [7:0] acc,
                output logic carry);
    logic [7:0] opnd, sum, next;

    mux8    m0 (.s(selb), .a(imm),  .b(din),  .y(opnd));
    addsub8 a0 (.sub(sub), .a(acc), .b(opnd), .y(sum), .cout(carry));
    mux8    m1 (.s(ld),   .a(sum),  .b(din),  .y(next));
    reg8    r0 (.clk(clk), .rst_n(rst_n), .en(1'b1), .d(next), .q(acc));
endmodule
