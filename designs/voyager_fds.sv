// voyager_fds.sv -- Voyager Flight Data Subsystem Processor
//
// The computer that left the solar system.
//
// Voyager 1 launched September 5, 1977. Voyager 2 launched
// August 20, 1977. Both are still operating. Voyager 1 is
// currently 24 billion kilometres away in interstellar space.
// The signal takes 22 hours to arrive. The computer runs at
// 806.4 kHz with 8K words of memory.
//
// Every photograph of Jupiter's Great Red Spot. Every reading
// from Saturn's rings. Every measurement of the heliopause.
// All processed by this instruction set.
//
// Based on: JPL Interoffice Memo MJS 2.64A
// "MJS FDS Processor Architecture and Instruction Set"
// J. Wooddell, 7 October 1974
//
// Reconstructed from the emulator at /c/dev/misc/Voyager.
// This is a synthesisable core — no I/O or DMA, just the
// CPU data path, registers, and instruction decoder.
// Add memory and I/O externally.
//
// To synthesise to SKY130:
//   ./takahe --lib sky130.lib --map voyager.v --sta 1 voyager_fds.sv
//
// (1 MHz is faster than the original's 806.4 kHz.
//  Voyager would approve. Probably.)

module voyager_fds (
    input  logic        clk,
    input  logic        rst_n,

    // Memory interface
    output logic [12:0] mem_addr,   // 13-bit address (8K)
    output logic [15:0] mem_wdata,  // write data
    input  logic [15:0] mem_rdata,  // read data
    output logic        mem_we,     // write enable
    output logic        mem_re,     // read enable

    // Status
    output logic        halted,
    output logic [12:0] pc_out      // program counter (debug)
);

    // ---- Registers ----
    // RA: accumulator (the workhorse)
    // RB: auxiliary register (operand for ALU)
    // IB: instruction buffer (current instruction)
    // PR: program register (13-bit PC)

    logic [15:0] ra, rb, ib;
    logic [12:0] pr;
    logic        flag_z, flag_p, flag_c;  // zero, positive, carry
    logic        bank_j, bank_a;          // jump/addr bank select

    // ---- Instruction fields ----
    logic [3:0]  opcode;
    logic [11:0] addr12;
    logic [12:0] eff_addr;
    logic [1:0]  bits_11_10;
    logic [1:0]  bits_9_8;

    assign opcode     = ib[15:12];
    assign addr12     = ib[11:0];
    assign bits_11_10 = ib[11:10];
    assign bits_9_8   = ib[9:8];
    assign pc_out     = pr;

    // ---- State machine ----
    // Simple 3-state: FETCH → DECODE → EXECUTE
    // The original was microcoded. We use a state machine
    // because it's clearer and synthesises well.

    typedef enum logic [1:0] {
        S_FETCH  = 2'b00,
        S_EXEC   = 2'b01,
        S_STORE  = 2'b10,
        S_HALT   = 2'b11
    } state_t;

    state_t state;

    // ---- Effective address with banking ----
    always_comb begin
        if (addr12 >= 12'hF80)
            eff_addr = {1'b0, addr12};  // special regs: no banking
        else if (bank_a)
            eff_addr = {1'b1, addr12};  // upper bank
        else
            eff_addr = {1'b0, addr12};  // lower bank
    end

    // ---- Main state machine ----
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ra     <= 16'h0000;
            rb     <= 16'h0000;
            ib     <= 16'h0000;
            pr     <= 13'h0000;
            flag_z <= 1'b1;
            flag_p <= 1'b0;
            flag_c <= 1'b0;
            bank_j <= 1'b0;
            bank_a <= 1'b0;
            halted <= 1'b0;
            state  <= S_FETCH;
            mem_we <= 1'b0;
            mem_re <= 1'b0;
            mem_addr  <= 13'h0000;
            mem_wdata <= 16'h0000;
        end else begin
            mem_we <= 1'b0;
            mem_re <= 1'b0;

            case (state)

            // ---- FETCH: read instruction at PR ----
            S_FETCH: begin
                mem_addr <= pr;
                mem_re   <= 1'b1;
                state    <= S_EXEC;
            end

            // ---- EXECUTE: decode and run ----
            S_EXEC: begin
                ib <= mem_rdata;
                pr <= pr + 1;

                case (mem_rdata[15:12])

                // 0000 — JMP: jump to address
                4'h0: begin
                    if (bank_j)
                        pr <= {1'b1, mem_rdata[11:0]};
                    else
                        pr <= {1'b0, mem_rdata[11:0]};
                end

                // 0001 — SRB: save RB to memory
                4'h1: begin
                    mem_addr  <= eff_addr;
                    mem_wdata <= rb;
                    mem_we    <= 1'b1;
                end

                // 0100 — MLD: load memory to RA
                4'h4: begin
                    mem_addr <= eff_addr;
                    mem_re   <= 1'b1;
                    state    <= S_STORE;  // need extra cycle for read
                end

                // 0101 — MRD: read memory to RB
                4'h5: begin
                    mem_addr <= eff_addr;
                    mem_re   <= 1'b1;
                    // RB loaded next cycle
                    state    <= S_STORE;
                end

                // 1000 — ABS: absolute entry (load address to RA)
                4'h8: begin
                    ra <= {4'h0, mem_rdata[11:0]};
                    flag_z <= (mem_rdata[11:0] == 12'h0);
                    flag_p <= 1'b1; // address is always positive
                end

                // 1001 — ALU operations
                4'h9: begin
                    if (mem_rdata[11:10] == 2'b00) begin
                        // Arithmetic: ADD, XOR, AND, OR
                        case (mem_rdata[9:8])
                        2'b00: begin // ADD
                            {flag_c, ra} <= {1'b0, ra} + {1'b0, rb};
                        end
                        2'b01: begin // XOR
                            ra <= ra ^ rb;
                        end
                        2'b10: begin // AND
                            ra <= ra & rb;
                        end
                        2'b11: begin // OR
                            ra <= ra | rb;
                        end
                        endcase
                    end else if (mem_rdata[11:10] == 2'b01) begin
                        if (mem_rdata[9] == 1'b0) begin
                            // SUB: RA = RA - RB
                            {flag_c, ra} <= {1'b0, ra} - {1'b0, rb};
                        end
                        // Shifts handled separately (complex encoding)
                    end
                    // Update flags after ALU
                    flag_z <= (ra == 16'h0);
                    flag_p <= (ra[15] == 1'b0) && (ra != 16'h0);
                end

                // 0011 — WAT: wait (halt until interrupt)
                4'h3: begin
                    state <= S_HALT;
                end

                default: begin
                    // Unimplemented opcodes: continue
                end

                endcase

                if (state != S_STORE && state != S_HALT)
                    state <= S_FETCH;
            end

            // ---- STORE: complete memory read ----
            S_STORE: begin
                // MLD reads into RA, MRD reads into RB
                if (ib[15:12] == 4'h4) begin
                    ra <= mem_rdata;
                    flag_z <= (mem_rdata == 16'h0);
                    flag_p <= (mem_rdata[15] == 1'b0) && (mem_rdata != 16'h0);
                end else begin
                    rb <= mem_rdata;
                end
                state <= S_FETCH;
            end

            // ---- HALT: stopped ----
            S_HALT: begin
                halted <= 1'b1;
            end

            endcase
        end
    end

endmodule
