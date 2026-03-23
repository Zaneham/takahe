# Takahe

Universal hardware synthesis tool supporting binary, ternary, stochastic, quantum, duodecimal, nucleotide, epistemic, cellular automata, affective, I Ching, chromatic music theory, particle physics, and Arrow's impossibility theorem.

Named after the takahē (*Porphyrio hochstetteri*), declared extinct in 1898 and rediscovered alive in the Murchison Mountains in 1948. 

## What It Does

Takes SystemVerilog and VHDL source and produces gate-level netlists mapped to real foundry cells:

```bash
# Binary synthesis to SKY130 130nm
./takahe --lib sky130.lib --map counter.v design.sv

# Ternary synthesis (balanced ternary, à la Setun)
./takahe --radix 3 --opt --parse design.sv

# Duodecimal synthesis (Mesopotamian base-12)
./takahe --radix 12 --opt --parse design.sv
```

PicoRV32 (a complete RISC-V CPU core) synthesises to 3,305 SKY130 gate instances. Zero parse errors. Zero multi-driver nets.

## The Cell Definition Architecture

Every computing paradigm is a `.def` file containing truth tables. The engine doesn't know what a NAND gate is; it reads the truth table and evaluates it. When you want a new paradigm you write a text file, not C code.

```
# Binary AND
cell AND radix 2 inputs 2 outputs 1
  truth 0 0 -> 0
  truth 0 1 -> 0
  truth 1 0 -> 0
  truth 1 1 -> 1
end

# Ternary AND (min) — balanced ternary {-1, 0, +1}
cell AND radix 3 inputs 2 outputs 1
  truth -1  0 -> -1
  truth  0  1 ->  0
  truth  1  1 ->  1
  ...
end
```

The optimiser is radix-aware. Ternary constant propagation, identity detection, and dead cell elimination all work through truth table evaluation rather than hardcoded rules for any particular paradigm.

## Thirteen Paradigms

| File | Paradigm | Radix | Origin |
|------|----------|-------|--------|
| `cells.def` | Binary | 2 | Shannon, 1938 |
| `cells_ter.def` | Balanced ternary | 3 | Brusentsov, 1958 |
| `cells_stoch.def` | Stochastic | 2* | Gaines, 1969 |
| `cells_qc.def` | Quantum (Clifford) | 2 | Feynman, 1982 |
| `cells_doz.def` | Duodecimal | 12 | Sumer, 3000 BCE |
| `cells_dna.def` | Nucleotide | 4 | LUCA, 3.7 Gya |
| `cells_epist.def` | Epistemic | 7 | Bochvar, 1938 |
| `cells_life.def` | Cellular automata | 2 | Conway/Wolfram |
| `cells_affect.def` | Affective | 8 | Plutchik, 1980 |
| `cells_iching.def` | I Ching trigrams | 8 | 伏羲, ~3000 BCE |
| `cells_music.def` | Music theory | 12 | Pythagoras/Bach |
| `cells_quark.def` | Particle physics | 6 | Gell-Mann, 1964 |
| `cells_arrow.def` | Impossibility | 6 | Arrow, 1951 |

\* Stochastic cells use binary hardware with probabilistic semantics: AND = multiplication, MUX = weighted addition.

### Highlights

**Ternary**: Negation is free because you just flip every trit, no two's complement needed. The 3-way MUX selects from three inputs with one control signal. Brusentsov proved this was better in 1958 and the Soviet Union cancelled it anyway.

**Quantum**: CNOT, Toffoli, Fredkin, Hadamard, the full Clifford gate set. The classical control plane for a quantum processor synthesises alongside the binary logic so one tool handles both domains.

**DNA**: Watson-Crick complement serves as the NOT gate, the CODON cell maps three nucleotides to an amino acid index, and the MATCH cell detects base pairing. Your body runs 37 trillion instances of this cell library.

**Epistemic**: Seven values from Bochvar's three-valued logic covering not just true and false but *justified* true, *believed* true, and *defeated* true. The CONSENSUS gate merges knowledge from multiple sources while the DEFEAT gate revokes warrants when counter-evidence arrives.

**Duodecimal**: The Mesopotamians counted in base-12 five thousand years ago and we still use their system for hours, months, and music. The half-adder correctly computes 7 sheep + 8 sheep = 3 sheep carry 1 dozen.

**Arrow's impossibility**: A `.def` file that documents its own impossibility. The FAIR voting cell is commented out because Arrow proved in 1951 that no truth table satisfying all three fairness axioms can exist, and the file contains the proof.

**I Ching**: Closes the historical loop. Leibniz saw the trigrams in 1703 and recognised binary arithmetic, Shannon formalised it in 1938, and Takahe generalised beyond it in 2026. The I Ching was a truth table lookup three millennia before anyone called it that.

## Four PDK Targets

| PDK | Node | Status |
|-----|------|--------|
| SKY130 | 130nm | Fully supported, five processors synthesised through OpenROAD |
| IHP SG13G2 | 130nm BiCMOS | Supported |
| GF180MCU | 180nm | Supported |
| ASAP7 | 7nm (predictive) | Supported |

## Tested Designs

All synthesise with zero parse errors and zero multi-driver nets:

| Design | Type | Cells | Source |
|--------|------|-------|--------|
| **PicoRV32** | RISC-V CPU | 3,305 | [cliffordwolf/picorv32](https://github.com/cliffordwolf/picorv32) |
| **Voyager FDS** | Flight computer | 210 | `designs/voyager_fds.sv` |
| **Minuteman D17B** | Guidance computer | 231 | `designs/minuteman_d17b.sv` |
| **Dozenal ALU** | Base-12 arithmetic | 41 | `designs/dozenal_alu.sv` |
| **Ruru** | Probabilistic processor | 157 | `designs/ruru.sv` |
| **qsim** | Quantum gate simulator | 59 | `designs/qsim.sv` |
| **Setun-70** | Ternary processor | 153 | `designs/setun70.sv` |
| **VHDL ALU** | 8-bit ALU (VHDL) | 107 | `designs/vhdl_alu.vhd` |

## Pipeline

```
SystemVerilog ─┐
               ├→ lex → parse → elaborate → lower → optimise → bit-blast → map → emit
VHDL ──────────┘   ↑                                    ↑                     ↑
              sv_tok.def                           cells_*.def            *.lib
              vhdl_tok.def
```

## Building

```bash
make            # build takahe
make test       # run tests (70 tests, zero failures)
make clean      # clean
```

Requires GCC and nothing else. Zero dependencies beyond libc. C99.

## CLI

```
takahe [flags] <source.sv|.vhd>

  --vhdl          VHDL mode (IEEE 1076-2008)
  --lex           dump tokens
  --parse         dump AST + RTL
  --opt           optimise (cprop + pattern match + DCE)
  --blif <f>      emit BLIF netlist
  --yosys <f>     emit Yosys JSON netlist
  --lib <f>       Liberty .lib cell library
  --map <f>       emit mapped gate-level Verilog
  --sta <mhz>     static timing analysis at target frequency
  --radix <n>     synthesis radix (2=binary, 3=ternary, 12=dozenal...)
  --lang <en|mi>  message language (en=English, mi=Te Reo Māori)
  --defs <f>      path to sv_tok.def
```

### Full pipeline example

```bash
# Synthesise, map to SKY130, run STA at 100MHz
./takahe --lib sky130.lib --map out.v --sta 100 design.sv

# Same thing, but the errors are in Te Reo Māori
./takahe --lib sky130.lib --map out.v --sta 100 --lang mi design.sv

# Ternary synthesis
./takahe --radix 3 --opt --parse design.sv
```

## ABEND Dumps and Error Codes

When Takahe hits an unrecoverable error it produces a structured ABEND dump in the mainframe tradition, showing exactly what the tool was doing when it failed. The pattern comes from IBM's CICS (1968): if you're going to crash, crash informatively.

```
╔══════════════════════════════════════╗
║     TAKAHE ABEND — HE HAPA NUKU     ║
╚══════════════════════════════════════╝
  Module:   tk_sta
  Reason:   net pool exhausted during STA forward pass
  Nets:     41 / 100
  Cells:    36 / 100
  Strings:  1 / 1048576 bytes
  Memories: 2

  The takahē was declared extinct in 1898.
  It came back. So can your design.
```

In Te Reo Māori (`--lang mi`):

```
  Wāhanga:   tk_sta
  Take:      kua pau te pūrere whatunga i te wā STA
  ...
  Kia kaha. Ka oti. Ka pai.
  (Be strong. It will be finished. It will be good.)
```
adding your own language is just adding another .txt file. Theoretically you could have an error dump in ancient sumerian while making a duodecimal chip, how neat!

### Error codes

All error messages use structured codes (`TK001`–`TK099`) loaded from text files in `lang/`. To add a new language, create `lang/<code>.txt` with one message per line.

| Range | Category |
|-------|----------|
| TK001–009 | Lexer and parser errors |
| TK010–019 | Elaboration (parameters, width) |
| TK020–029 | RTL lowering (pool exhaustion, no driver) |
| TK030–039 | Timing (setup/hold violations, combinational loops) |
| TK040–049 | Mapping (bit-blast overflow, Espresso limits) |
| TK090–099 | ABEND dump fields |

### Transaction journaling

Every optimisation pass is protected by a CICS-style transaction journal. If a pass fails or makes things worse the netlist rolls back to its pre-pass state, no half-optimised netlists ever. The journal records every cell addition, deletion, and modification, and rollback replays the journal in reverse like rewinding a tape except the tape is your netlist.

## Project Structure

```
src/
├── main.c       CLI entry point
├── tk_abend.c   ABEND dumps + bilingual messages
├── tk_jrn.c     CICS-style transaction journal
├── lex/         SV + VHDL lexers, preprocessor, .def loader
├── parse/       SV + VHDL recursive descent parsers
├── elab/        constant eval, elaboration, width inference
├── rtl/         RTL IR + AST-to-RTL lowering
├── opt/         constant propagation, dead cell elimination
├── xform/       bit-blast, pattern matching, Espresso minimiser
├── tech/        Liberty parser, cell defs, PCHIP, STA, timing-driven opt
└── emit/        BLIF, Yosys JSON, gate-level Verilog output

defs/            13 computing paradigm definitions
lang/            bilingual message catalogues (en, mi)
docs/            situation reports, audit logs
```

## Quality Notice

This cell library carries NO WARRANTY regarding copper quality. If your interconnect is of substandard grade you have only yourself to blame for buying from Ea-nasir. You were warned. Nanni was warned. Nobody listens.



## Support the Takahē

This tool is named after the takahē, a flightless bird declared extinct in 1898 and rediscovered alive in New Zealand's Murchison Mountains in 1948. The population is still under 500. DOC's Takahē Recovery Programme is the reason the bird exists today.

If this project is useful to you, consider helping keep them alive.

**[Donate to the Takahē Recovery Programme](https://www.doc.govt.nz/our-work/takahe-recovery-programme/donate/)**

## License

MPL 2.0. Modify Takahe's files and share your modifications, build around it and your code stays yours. Because chip design shouldn't cost more than a house and alternative computing shouldn't be locked behind anyone's paywall.
