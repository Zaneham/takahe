// sv_coverage.sv -- probe for missing SV features
// Each module tests one construct. If it parses, it works.

// 1. Ternary operator
module t_tern (
    input  logic       sel,
    input  logic [7:0] a, b,
    output logic [7:0] y
);
    assign y = sel ? a : b;
endmodule

// 2. Multi-dimensional arrays
module t_arr (
    input  logic        clk,
    input  logic [3:0]  addr,
    input  logic [7:0]  din,
    output logic [7:0]  dout
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr] <= din;
        dout <= mem[addr];
    end
endmodule

// 3. For loop in always_comb
module t_loop (
    input  logic [7:0] a,
    output logic [3:0] count
);
    integer i;
    always_comb begin
        count = 0;
        for (i = 0; i < 8; i = i + 1)
            if (a[i]) count = count + 1;
    end
endmodule

// 4. Concatenation and replication
module t_cat (
    input  logic [3:0] a,
    output logic [7:0] y,
    output logic [15:0] z
);
    assign y = {a, 4'b0000};
    assign z = {4{a}};
endmodule

// 5. Signed arithmetic
module t_sign (
    input  logic signed [7:0] a, b,
    output logic signed [8:0] sum
);
    assign sum = a + b;
endmodule

// 6. Localparam with expressions
module t_param #(
    parameter WIDTH = 8,
    parameter DEPTH = 256
)(
    input  logic [WIDTH-1:0]       din,
    input  logic [$clog2(DEPTH)-1:0] addr,
    output logic [WIDTH-1:0]       dout
);
    localparam ADDR_W = $clog2(DEPTH);
    logic [WIDTH-1:0] mem [0:DEPTH-1];
    assign dout = mem[addr];
endmodule

// 7. Enum
module t_enum (
    input  logic       clk, rst,
    output logic [1:0] state
);
    typedef enum logic [1:0] {
        IDLE = 2'b00,
        RUN  = 2'b01,
        DONE = 2'b10
    } state_t;

    state_t s;
    always_ff @(posedge clk)
        if (rst) s <= IDLE;
        else if (s == IDLE) s <= RUN;
        else if (s == RUN) s <= DONE;
        else s <= IDLE;

    assign state = s;
endmodule

// 8. Multi-bit part select
module t_part (
    input  logic [31:0] word,
    output logic [7:0]  byte0, byte1, byte2, byte3
);
    assign byte0 = word[7:0];
    assign byte1 = word[15:8];
    assign byte2 = word[23:16];
    assign byte3 = word[31:24];
endmodule

// 9. Nested if-else in always_ff
module t_nest (
    input  logic       clk,
    input  logic [1:0] sel,
    input  logic [7:0] a, b, c, d,
    output logic [7:0] y
);
    always_ff @(posedge clk) begin
        if (sel == 2'b00)
            y <= a;
        else if (sel == 2'b01)
            y <= b;
        else if (sel == 2'b10)
            y <= c;
        else
            y <= d;
    end
endmodule

// 10. Module instantiation with named ports
module t_sub (
    input  logic a, b,
    output logic y
);
    assign y = a & b;
endmodule

module t_inst (
    input  logic x1, x2,
    output logic z
);
    t_sub u0 (
        .a(x1),
        .b(x2),
        .y(z)
    );
endmodule

// 11. Casez
module t_casez (
    input  logic [3:0] in,
    output logic [1:0] out
);
    always_comb begin
        casez (in)
            4'b1???: out = 2'b11;
            4'b01??: out = 2'b10;
            4'b001?: out = 2'b01;
            default: out = 2'b00;
        endcase
    end
endmodule

// 12. Generate if
module t_genif #(
    parameter USE_FAST = 1
)(
    input  logic [7:0] a, b,
    output logic [7:0] y
);
    generate
        if (USE_FAST) begin : fast
            assign y = a + b;
        end else begin : slow
            assign y = a | b;
        end
    endgenerate
endmodule
