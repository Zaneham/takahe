/* sram.sv -- Simple sync-write, sync-read RAM for memory-mapper testing
 * Smallest realistic memory pattern: gated write, indexed read,
 * no nested control flow. The lowerer recognises both access
 * patterns and produces RT_MEMRD/RT_MEMWR cells that the
 * FPGA backend can absorb into one SB_RAM40_4K instance. */

module sram (
    input  logic        clk,
    input  logic        we,
    input  logic [3:0]  addr,
    input  logic [7:0]  din,
    output logic [7:0]  dout
);
    logic [7:0] mem [0:15];

    always_ff @(posedge clk) begin
        if (we) mem[addr] <= din;
    end

    assign dout = mem[addr];
endmodule
