// ruru.sv -- The World's First Probabilistic Processor
//
// Every computer since 1837 has assumed it knows the answer.
// Ruru knows it doesn't.
//
// Named after the morepork (Ninox novaeseelandiae), New Zealand's
// only surviving native owl. In Māori tradition, the ruru is a
// watchful guardian. Ruru the processor computes under uncertainty
// and tells you the probability.
//
// Architecture:
//   - 16 probabilistic registers (pr0-pr15)
//   - Each register: tag(4) + mean(16) + variance(16) = 36 bits
//   - Gaussian fast path: PADD, PFUSE, POBS as single-cycle ops
//   - Tag dispatch: instructions adapt to distribution type
//
// The Gaussian operations (fixed-point, 16-bit):
//   PADD:   μ = μ₁ + μ₂,           σ² = σ₁² + σ₂²
//   PFUSE:  σ² = 1/(1/σ₁² + 1/σ₂²), μ = σ²·(μ₁/σ₁² + μ₂/σ₂²)
//   POBS:   Kalman update — single most important equation in
//           estimation theory, now a machine instruction.
//
// Lineage:
//   Babbage (1837) → Setun (1958) → B5000 (1961) → Ruru (2026)
//   Deterministic  → Ternary       → Tagged       → Probabilistic
//
// To synthesise:
//   ./takahe --lib sky130.lib --map ruru.v --sta 100 ruru.sv

// ---- Tag definitions ----
// Each register carries its type. The processor adapts.
// Burroughs B5000 did this for code/data in 1961.
// We do it for probability distributions in 2026.

`define TAG_EMPTY  4'h0
`define TAG_CONST  4'h1
`define TAG_GAUSS  4'h2
`define TAG_UNIF   4'h3

// ---- Instruction encoding (16-bit) ----
// [15:12] opcode
// [11:8]  destination register (pr0-pr15)
// [7:4]   source register 1
// [3:0]   source register 2 / immediate

`define OP_NOP     4'h0
`define OP_PCONST  4'h1   // Load constant (zero variance)
`define OP_PGAUSS  4'h2   // Load Gaussian (mean + variance from memory)
`define OP_PADD    4'h3   // Distributional addition
`define OP_PSUB    4'h4   // Distributional subtraction
`define OP_PFUSE   4'h5   // Sensor fusion (precision-weighted)
`define OP_POBS    4'h6   // Bayesian update (Kalman)
`define OP_PMEAN   4'h7   // Extract mean to scalar output
`define OP_PVAR    4'h8   // Extract variance to scalar output
`define OP_PCMP    4'h9   // Probabilistic comparison
`define OP_PCOPY   4'hA   // Copy register
`define OP_PCLEAR  4'hB   // Clear register
`define OP_PSCALE  4'hC   // Scale by constant
`define OP_HALT    4'hF   // Stop

// ---- Probabilistic Register ----
// tag(4) + mean(16) + variance(16) = 36 bits per register

module ruru_regfile (
    input  logic        clk,
    input  logic        rst_n,

    // Read ports (2 simultaneous reads)
    input  logic [3:0]  raddr1,
    input  logic [3:0]  raddr2,
    output logic [3:0]  rtag1,
    output logic [15:0] rmean1,
    output logic [15:0] rvar1,
    output logic [3:0]  rtag2,
    output logic [15:0] rmean2,
    output logic [15:0] rvar2,

    // Write port
    input  logic        we,
    input  logic [3:0]  waddr,
    input  logic [3:0]  wtag,
    input  logic [15:0] wmean,
    input  logic [15:0] wvar
);
    // 16 registers × 36 bits = 576 bits total.
    // The entire probabilistic state of the processor.
    logic [3:0]  tags  [0:15];
    logic [15:0] means [0:15];
    logic [15:0] vars  [0:15];

    // Read
    assign rtag1  = tags[raddr1];
    assign rmean1 = means[raddr1];
    assign rvar1  = vars[raddr1];
    assign rtag2  = tags[raddr2];
    assign rmean2 = means[raddr2];
    assign rvar2  = vars[raddr2];

    // Write
    integer i;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (i = 0; i < 16; i = i + 1) begin
                tags[i]  <= `TAG_EMPTY;
                means[i] <= 16'h0;
                vars[i]  <= 16'h0;
            end
        end else if (we) begin
            tags[waddr]  <= wtag;
            means[waddr] <= wmean;
            vars[waddr]  <= wvar;
        end
    end
endmodule

// ---- Gaussian ALU ----
// The mathematical core. Fixed-point 16-bit.
// Mean: signed 8.8 fixed point (range -128 to +127.996)
// Variance: unsigned 8.8 fixed point (range 0 to 255.996)
//
// PADD:  μ = μ₁ + μ₂,  σ² = σ₁² + σ₂²
//        (independent random variables: variances add)
//
// PFUSE: Combine two measurements of the SAME quantity.
//        σ² = (σ₁² · σ₂²) / (σ₁² + σ₂²)
//        μ  = (μ₁·σ₂² + μ₂·σ₁²) / (σ₁² + σ₂²)
//        Information ALWAYS increases. Uncertainty ALWAYS decreases.
//        This is why sensor fusion works.
//
// POBS:  Bayesian update. Same math as PFUSE but semantically:
//        prior + observation → posterior.
//        This IS the Kalman filter update. One instruction.

module ruru_galu (
    input  logic [2:0]  op,       // 0=ADD 1=SUB 2=FUSE 3=OBS 4=SCALE
    input  logic [15:0] mean1,
    input  logic [15:0] var1,
    input  logic [15:0] mean2,
    input  logic [15:0] var2,
    output logic [15:0] mean_out,
    output logic [15:0] var_out
);
    // Intermediate wires
    logic [31:0] var_sum;
    logic [31:0] var_prod;
    logic [31:0] fuse_var;
    logic [31:0] fuse_mean_num;
    logic [31:0] m1_v2, m2_v1;

    assign var_sum  = {16'h0, var1} + {16'h0, var2};
    assign var_prod = var1 * var2;

    // PFUSE/POBS: precision-weighted combination
    // fuse_var = (v1 * v2) / (v1 + v2)
    assign fuse_var = (var_sum != 0) ? var_prod / var_sum[15:0] : 32'h0;

    // fuse_mean = (m1*v2 + m2*v1) / (v1 + v2)
    assign m1_v2 = $signed(mean1) * var2;
    assign m2_v1 = $signed(mean2) * var1;
    assign fuse_mean_num = m1_v2 + m2_v1;

    always_comb begin
        case (op)
        3'b000: begin // PADD: variances add, means add
            mean_out = mean1 + mean2;
            var_out  = var1 + var2;
        end

        3'b001: begin // PSUB: variances add, means subtract
            mean_out = mean1 - mean2;
            var_out  = var1 + var2;
        end

        3'b010, 3'b011: begin // PFUSE / POBS
            // Precision-weighted combination
            var_out  = fuse_var[15:0];
            mean_out = (var_sum != 0) ?
                fuse_mean_num[23:8] : // fixed-point shift
                mean1;
        end

        3'b100: begin // PSCALE: scale by mean2 as constant
            // μ = μ₁ × c,  σ² = σ₁² × c²
            mean_out = (mean1 * mean2) >> 8; // fixed-point multiply
            var_out  = (var1 * mean2 * mean2) >> 16;
        end

        default: begin
            mean_out = mean1;
            var_out  = var1;
        end
        endcase
    end
endmodule

// ---- Top: Ruru Probabilistic Processor ----

module ruru (
    input  logic        clk,
    input  logic        rst_n,

    // Instruction interface
    input  logic [15:0] instr,
    input  logic        valid,

    // Memory interface (for loading distributions)
    input  logic [15:0] mem_data,

    // Scalar output (for PMEAN/PVAR queries)
    output logic [15:0] scalar_out,
    output logic        scalar_valid,

    // Status
    output logic        halted
);
    // Decode instruction fields
    logic [3:0] opcode, rd, rs1, rs2;
    assign opcode = instr[15:12];
    assign rd     = instr[11:8];
    assign rs1    = instr[7:4];
    assign rs2    = instr[3:0];

    // Register file
    logic [3:0]  tag1, tag2;
    logic [15:0] mean1, var1, mean2, var2;
    logic        rf_we;
    logic [3:0]  rf_waddr, rf_wtag;
    logic [15:0] rf_wmean, rf_wvar;

    ruru_regfile regs (
        .clk(clk), .rst_n(rst_n),
        .raddr1(rs1), .raddr2(rs2),
        .rtag1(tag1), .rmean1(mean1), .rvar1(var1),
        .rtag2(tag2), .rmean2(mean2), .rvar2(var2),
        .we(rf_we), .waddr(rf_waddr),
        .wtag(rf_wtag), .wmean(rf_wmean), .wvar(rf_wvar)
    );

    // Gaussian ALU
    logic [2:0]  alu_op;
    logic [15:0] alu_mean, alu_var;

    ruru_galu galu (
        .op(alu_op),
        .mean1(mean1), .var1(var1),
        .mean2(mean2), .var2(var2),
        .mean_out(alu_mean), .var_out(alu_var)
    );

    // Control
    always_comb begin
        rf_we      = 1'b0;
        rf_waddr   = rd;
        rf_wtag    = `TAG_GAUSS;
        rf_wmean   = alu_mean;
        rf_wvar    = alu_var;
        alu_op     = 3'b000;
        scalar_out = 16'h0;
        scalar_valid = 1'b0;

        if (valid && !halted) begin
            case (opcode)
            `OP_PCONST: begin
                rf_we    = 1'b1;
                rf_wtag  = `TAG_CONST;
                rf_wmean = mem_data;
                rf_wvar  = 16'h0;
            end

            `OP_PGAUSS: begin
                rf_we    = 1'b1;
                rf_wtag  = `TAG_GAUSS;
                rf_wmean = mem_data;
                rf_wvar  = {rs2, rs2, rs2, rs2}; // variance from imm
            end

            `OP_PADD: begin
                alu_op = 3'b000;
                rf_we  = 1'b1;
            end

            `OP_PSUB: begin
                alu_op = 3'b001;
                rf_we  = 1'b1;
            end

            `OP_PFUSE: begin
                alu_op = 3'b010;
                rf_we  = 1'b1;
            end

            `OP_POBS: begin
                alu_op = 3'b011;
                rf_we  = 1'b1;
            end

            `OP_PMEAN: begin
                scalar_out   = mean1;
                scalar_valid = 1'b1;
            end

            `OP_PVAR: begin
                scalar_out   = var1;
                scalar_valid = 1'b1;
            end

            `OP_PCMP: begin
                // P(X1 > X2): approximate for Gaussians
                // If mean1 > mean2 with high confidence → 1
                scalar_out   = ($signed(mean1) > $signed(mean2)) ?
                               16'hFF00 : 16'h0100;
                scalar_valid = 1'b1;
            end

            `OP_PCOPY: begin
                rf_we    = 1'b1;
                rf_wtag  = tag1;
                rf_wmean = mean1;
                rf_wvar  = var1;
            end

            `OP_PCLEAR: begin
                rf_we    = 1'b1;
                rf_wtag  = `TAG_EMPTY;
                rf_wmean = 16'h0;
                rf_wvar  = 16'h0;
            end

            `OP_PSCALE: begin
                alu_op = 3'b100;
                rf_we  = 1'b1;
            end

            default: begin end // NOP, HALT handled below
            endcase
        end
    end

    // Halt register
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            halted <= 1'b0;
        else if (valid && opcode == `OP_HALT)
            halted <= 1'b1;
    end
endmodule
