// sv_advanced.sv -- harder SV constructs

// 1. Interfaces
interface axi_lite_if #(parameter ADDR_W = 32, DATA_W = 32);
    logic [ADDR_W-1:0] awaddr;
    logic               awvalid;
    logic               awready;
    logic [DATA_W-1:0] wdata;
    logic               wvalid;
    logic               wready;
    modport master (output awaddr, awvalid, wdata, wvalid,
                    input  awready, wready);
    modport slave  (input  awaddr, awvalid, wdata, wvalid,
                    output awready, wready);
endinterface

// 2. Struct packed
module t_struct (
    input  logic        clk,
    input  logic [15:0] data_in,
    output logic [15:0] data_out
);
    typedef struct packed {
        logic [7:0] hi;
        logic [7:0] lo;
    } word_t;

    word_t r;
    always_ff @(posedge clk) begin
        r.hi <= data_in[15:8];
        r.lo <= data_in[7:0];
    end
    assign data_out = {r.hi, r.lo};
endmodule

// 3. Unique case
module t_ucase (
    input  logic [2:0] sel,
    output logic [7:0] y
);
    always_comb begin
        unique case (sel)
            3'd0: y = 8'h01;
            3'd1: y = 8'h02;
            3'd2: y = 8'h04;
            3'd3: y = 8'h08;
            3'd4: y = 8'h10;
            3'd5: y = 8'h20;
            3'd6: y = 8'h40;
            3'd7: y = 8'h80;
        endcase
    end
endmodule

// 4. Assign with reduction operators
module t_reduce (
    input  logic [7:0] a,
    output logic        and_all,
    output logic        or_all,
    output logic        xor_all
);
    assign and_all = &a;
    assign or_all  = |a;
    assign xor_all = ^a;
endmodule

// 5. Multiple always blocks driving different signals
module t_multi (
    input  logic       clk, rst,
    input  logic [7:0] a, b,
    output logic [7:0] sum,
    output logic [7:0] diff
);
    always_ff @(posedge clk)
        if (rst) sum <= 8'd0;
        else     sum <= a + b;

    always_ff @(posedge clk)
        if (rst) diff <= 8'd0;
        else     diff <= a - b;
endmodule

// 6. Wire with explicit width
module t_wire (
    input  logic [3:0] a, b,
    output logic [4:0] y
);
    wire [4:0] extended_a = {1'b0, a};
    wire [4:0] extended_b = {1'b0, b};
    assign y = extended_a + extended_b;
endmodule

// 7. Conditional generate with for
module t_genfor #(parameter N = 4)(
    input  logic [N-1:0] a,
    input  logic [N-1:0] b,
    output logic [N-1:0] y
);
    genvar i;
    generate
        for (i = 0; i < N; i = i + 1) begin : bit_and
            assign y[i] = a[i] & b[i];
        end
    endgenerate
endmodule

// 8. Task-like function
module t_func (
    input  logic [7:0] a,
    output logic [7:0] y
);
    function automatic logic [7:0] swap_nibbles(input logic [7:0] x);
        return {x[3:0], x[7:4]};
    endfunction

    assign y = swap_nibbles(a);
endmodule

// 9. Default port values
module t_defport (
    input  logic       clk,
    input  logic       en = 1'b1,
    input  logic [7:0] d,
    output logic [7:0] q
);
    always_ff @(posedge clk)
        if (en) q <= d;
endmodule

// 10. Shift operators
module t_shift (
    input  logic [15:0] a,
    input  logic [3:0]  shamt,
    output logic [15:0] lsl,
    output logic [15:0] lsr,
    output logic signed [15:0] asr
);
    assign lsl = a << shamt;
    assign lsr = a >> shamt;
    assign asr = $signed(a) >>> shamt;
endmodule
