// minuteman_d17b.sv -- Minuteman I Guidance Computer
//
// The computer that keeps 400 nuclear missiles pointed at things.
// Since 1962. Still in service. We are not making this up.
//
// Architecture (from D17B Computer Programming Manual, Sep 1971):
//   - 24-bit words, sign-magnitude arithmetic
//   - 4-bit opcode, channel/sector addressing
//   - Accumulator-based (single accumulator A)
//   - ~12,800 instructions/second
//   - Original weight: 28kg. This version: under 5,000 um2.
//
// Instruction format (24 bits):
//   [23:20] opcode (4 bits)
//   [19]    flag bit
//   [18:15] next sector pointer
//   [14:9]  channel address (operand location)
//   [8:2]   sector address
//
// This is a simplified core: ALU + accumulator + decoder.
// Memory interface is external (the original used a rotating
// magnetic disc at 6000 RPM because RAM hadn't been invented
// yet, or rather it had but it weighed too much for a missile).
//
// Based on: D17B Programming Manual (Sep 1971)
// Emulator: /c/dev/emulators/Hopper/minuteman-emu
//
// To synthesise:
//   ./takahe --lib sky130.lib --map minuteman.v --sta 1 minuteman_d17b.sv

module minuteman_d17b (
    input  logic        clk,
    input  logic        rst_n,

    // Memory interface (channel/sector addressing)
    output logic [12:0] mem_addr,    // channel(6) + sector(7)
    output logic [23:0] mem_wdata,
    input  logic [23:0] mem_rdata,
    output logic        mem_we,
    output logic        mem_re,

    // Status
    output logic        halted,
    output logic [23:0] acc_out      // accumulator (debug)
);

    // ---- Registers ----
    logic [23:0] acc;        // Accumulator (A register)
    logic [23:0] ib;         // Instruction buffer
    logic [12:0] pc;         // Program counter (channel + sector)
    logic        flag_neg;   // Negative flag (sign bit of acc)
    logic        flag_zero;  // Zero flag

    // ---- Instruction fields ----
    logic [3:0]  opcode;
    logic        flag_bit;
    logic [12:0] operand;    // channel[14:9] + sector[8:2]

    assign opcode   = ib[23:20];
    assign flag_bit = ib[19];
    assign operand  = ib[14:2];
    assign acc_out  = acc;

    // ---- Sign-magnitude helpers ----
    // The D17B used sign-magnitude because two's complement
    // hadn't been standardised yet. Like measuring in cubits:
    // technically valid, historically inevitable, and slightly
    // annoying for everyone who came after.

    wire [22:0] acc_mag  = acc[22:0];
    wire        acc_sign = acc[23];
    wire [22:0] rd_mag   = mem_rdata[22:0];
    wire        rd_sign  = mem_rdata[23];

    // ---- State machine ----
    typedef enum logic [1:0] {
        S_FETCH  = 2'b00,
        S_EXEC   = 2'b01,
        S_STORE  = 2'b10,
        S_HALT   = 2'b11
    } state_t;

    state_t state;

    // ---- Main state machine ----
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc      <= 24'h000000;
            ib       <= 24'h000000;
            pc       <= 13'h0000;
            flag_neg <= 1'b0;
            flag_zero<= 1'b1;
            halted   <= 1'b0;
            state    <= S_FETCH;
            mem_we   <= 1'b0;
            mem_re   <= 1'b0;
            mem_addr <= 13'h0000;
            mem_wdata<= 24'h000000;
        end else begin
            mem_we <= 1'b0;
            mem_re <= 1'b0;

            case (state)

            // ---- FETCH ----
            S_FETCH: begin
                mem_addr <= pc;
                mem_re   <= 1'b1;
                state    <= S_EXEC;
            end

            // ---- EXECUTE ----
            S_EXEC: begin
                ib <= mem_rdata;
                pc <= pc + 1;

                case (mem_rdata[23:20])

                // 1001 (44) — CLA: Clear and Add
                // A ← mem[operand]
                4'h9: begin
                    mem_addr <= mem_rdata[14:2];
                    mem_re   <= 1'b1;
                    state    <= S_STORE;
                end

                // 1101 (64) — ADD: Add to accumulator
                // A ← A + mem[operand]
                4'hD: begin
                    mem_addr <= mem_rdata[14:2];
                    mem_re   <= 1'b1;
                    state    <= S_STORE;
                end

                // 1011 (54) — STO: Store accumulator
                // mem[operand] ← A
                4'hB: begin
                    mem_addr  <= mem_rdata[14:2];
                    mem_wdata <= acc;
                    mem_we    <= 1'b1;
                    state     <= S_FETCH;
                end

                // 1010 (50) — TRA: Transfer (unconditional jump)
                4'hA: begin
                    pc    <= mem_rdata[14:2];
                    state <= S_FETCH;
                end

                // 0110 (30) — TMI: Transfer on Minus
                4'h6: begin
                    if (acc[23]) // sign bit set = negative
                        pc <= mem_rdata[14:2];
                    state <= S_FETCH;
                end

                // 0101 (24) — MPY: Multiply
                // A ← A × mem[operand] (simplified: lower 24 bits)
                4'h5: begin
                    mem_addr <= mem_rdata[14:2];
                    mem_re   <= 1'b1;
                    state    <= S_STORE;
                end

                default: begin
                    state <= S_FETCH;
                end

                endcase

                // Update flags
                flag_neg  <= acc[23];
                flag_zero <= (acc[22:0] == 23'h0);
            end

            // ---- STORE: complete memory read ----
            S_STORE: begin
                case (ib[23:20])

                4'h9: begin // CLA
                    acc <= mem_rdata;
                end

                4'hD: begin // ADD (sign-magnitude)
                    if (acc[23] == mem_rdata[23]) begin
                        // Same sign: add magnitudes, keep sign
                        acc <= {acc[23], acc[22:0] + mem_rdata[22:0]};
                    end else begin
                        // Different signs: subtract, sign of larger
                        if (acc[22:0] >= mem_rdata[22:0])
                            acc <= {acc[23], acc[22:0] - mem_rdata[22:0]};
                        else
                            acc <= {mem_rdata[23], mem_rdata[22:0] - acc[22:0]};
                    end
                end

                4'h5: begin // MPY (simplified)
                    acc <= {acc[23] ^ mem_rdata[23],
                            acc[10:0] * mem_rdata[10:0]};
                end

                default: begin
                    acc <= mem_rdata;
                end

                endcase

                flag_neg  <= acc[23];
                flag_zero <= (acc[22:0] == 23'h0);
                state     <= S_FETCH;
            end

            // ---- HALT ----
            S_HALT: begin
                halted <= 1'b1;
            end

            endcase
        end
    end

endmodule
