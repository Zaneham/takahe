// bigger.sv -- More complex SystemVerilog for parser testing
// A parameterised FIFO with generate blocks, structs, and enums.

typedef enum logic [1:0] {
    IDLE   = 2'b00,
    WRITE  = 2'b01,
    READ   = 2'b10,
    FULL   = 2'b11
} fifo_state_t;

typedef struct packed {
    logic [7:0]  data;
    logic        valid;
    logic        last;
} fifo_entry_t;

module fifo #(
    parameter DEPTH = 16,
    parameter WIDTH = 8
)(
    input  logic             clk,
    input  logic             rst_n,
    input  logic             wr_en,
    input  logic             rd_en,
    input  logic [WIDTH-1:0] wr_data,
    output logic [WIDTH-1:0] rd_data,
    output logic             empty,
    output logic             full
);

    localparam ADDR_W = $clog2(DEPTH);

    logic [WIDTH-1:0] mem [0:DEPTH-1];
    logic [ADDR_W:0]  wr_ptr, rd_ptr;
    logic [ADDR_W:0]  count;

    fifo_state_t state;

    assign empty = (count == '0);
    assign full  = (count == DEPTH[ADDR_W:0]);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ptr <= '0;
            rd_ptr <= '0;
            count  <= '0;
            state  <= IDLE;
        end else begin
            case ({wr_en, rd_en})
                2'b10: begin
                    if (!full) begin
                        mem[wr_ptr[ADDR_W-1:0]] <= wr_data;
                        wr_ptr <= wr_ptr + 1'b1;
                        count  <= count + 1'b1;
                    end
                end
                2'b01: begin
                    if (!empty) begin
                        rd_ptr <= rd_ptr + 1'b1;
                        count  <= count - 1'b1;
                    end
                end
                2'b11: begin
                    if (!full) begin
                        mem[wr_ptr[ADDR_W-1:0]] <= wr_data;
                        wr_ptr <= wr_ptr + 1'b1;
                    end
                    if (!empty) begin
                        rd_ptr <= rd_ptr + 1'b1;
                    end
                end
                default: ;
            endcase
        end
    end

    assign rd_data = mem[rd_ptr[ADDR_W-1:0]];

    // Generate block for parity checking
    generate
        if (WIDTH > 4) begin : parity_gen
            logic parity;
            assign parity = ^wr_data;
        end
    endgenerate

endmodule
