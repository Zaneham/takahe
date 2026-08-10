# Takahe

Hardware synthesis. SystemVerilog, VHDL and ABEL-HDL in, gate-level netlists mapped to real foundry cells out.

PicoRV32, a complete RISC-V CPU core, synthesises to 3,305 SKY130 gate instances with zero parse errors and zero multi-driver nets. Five processors have been through OpenROAD.

Named after the takahē (*Porphyrio hochstetteri*, also pronounced as Tah-Kah-Hey), declared extinct in 1898 and rediscovered alive in the Murchison Mountains in 1948.

This project started so I could learn how to make my own chips and understand Verilog better. I also wanted to do more historical reconstructions of old computers, which hopefully explains why it's built the way it is.

## What It Does

```bash
# Binary synthesis to SKY130 130nm
./takahe --lib sky130.lib --map counter.v design.sv

# VHDL
./takahe --vhdl --parse design.vhd

# ABEL-HDL (PLD designs from the 1980s-2000s)
./takahe --parse decoder.abl

# Ternary synthesis (balanced ternary, à la Setun)
./takahe --radix 3 --opt --parse design.sv
```

## Four PDK Targets

| PDK | Node | Status |
|-----|------|--------|
| SKY130 | 130nm | Fully supported, five processors synthesised through OpenROAD |
| IHP SG13G2 | 130nm BiCMOS | Supported |
| GF180MCU | 180nm | Supported |
| ASAP7 | 7nm (predictive) | Supported |

FPGAs too, via nextpnr JSON, currently Lattice iCE40 (4-LUT):

```bash
./takahe --opt --fpga output.json design.sv
# Then: nextpnr-ice40 --json output.json --pcf pins.pcf --asc out.asc
```

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


## Getting it

If you're lazy like me and just want to run and go, then you can install it [here](https://github.com/Zaneham/takahe/releases/latest). It comes with no dependencies and you don't have to run `make` to use it. There's a build for Linux, macOS and Windows.

```bash
tar xzf takahe-*-linux-x86_64.tar.gz
cd takahe-*-linux-x86_64
./takahe --parse design.sv
```

## Building

If you'd rather build it, or you're on something I don't ship a binary for (if that is the case please tell me I am so curious as to what you're using):

```bash
make            # build takahe
make test       # run tests (133 tests, zero failures)
make install    # /usr/local by default, honours PREFIX and DESTDIR
make clean      # clean
```

`make install` puts the binary in `<prefix>/bin`, the token and cell
definitions in `<prefix>/share/takahe`, and a CMake package config alongside,
so another project can `find_package(Takahe)` and synthesise designs as part
of its own build. See [docs/cmake.md](docs/cmake.md).

## CLI

```
takahe [flags] <source.sv|.vhd|.abl>

  --vhdl          VHDL mode (IEEE 1076-2008)
  --abel          ABEL-HDL mode (Data I/O, 1995)
  --lex           dump tokens
  --parse         dump AST + RTL
  --opt           optimise (cprop + pattern match + DCE)
  --equiv         equivalence check (pre-opt vs post-opt)
  --hash          print 64-bit fingerprint of synthesised netlist
  --budget <n>    refuse to emit if live cell count exceeds n
  --tmr           radiation hardening (triplicate DFFs + voters)
  --tmr-full      radiation hardening (triplicate everything)
  --fi            prove no single flop upset reaches an output
  --fi-comb       same, but faulting every gate output too
  --fpga <f>      emit nextpnr JSON for iCE40 FPGA
  --blif <f>      emit BLIF netlist
  --yosys <f>     emit Yosys JSON netlist
  --lib <f>       Liberty .lib cell library
  --map <f>       emit mapped gate-level Verilog
  --netlist       read a structural gate netlist (needs --lib)
  --seq           report recovered registers and shift chains
  --sta <mhz>     static timing analysis at target frequency
  --radix <n>     synthesis radix (2=binary, 3=ternary, 12=dozenal...)
  --lang <en|mi>  message language (en=English, mi=Te Reo Māori)
  --defs <f>      path to sv_tok.def
  --help          print full usage (also -h)
```

```bash
# Synthesise, map to SKY130, run STA at 100MHz
./takahe --lib sky130.lib --map out.v --sta 100 design.sv

# Same thing, but the errors are in Te Reo Māori
./takahe --lib sky130.lib --map out.v --sta 100 --lang mi design.sv
```

## What It Won't Tell You It Can Do

The SystemVerilog frontend scores **66% of the [sv-tests](https://github.com/chipsalliance/sv-tests) conformance suite**, 632 of 948 valid tests. CI gates on that number, so it can't quietly slide, and raising the floor is how progress gets recorded. The gaps cluster in the object-oriented and verification half of the language, worst at chapter 8 classes (1 of 44), which matters less for synthesis than the headline percentage suggests. Run it yourself:

```bash
git clone --depth 1 https://github.com/chipsalliance/sv-tests.git
tools/sv-tests.sh sv-tests
```

`--equiv` proves equivalence with a SAT miter where it can, and says so plainly where it can't. A design holding wide operators that were never bit-blasted, or latches, or memories, gets `not encodable for SAT` and falls back to random vectors, with that stated out loud rather than reported as a proof. So the formal result currently covers bit-level designs: recovered netlists, and anything already bit-blasted.

Timing analysis is topological, not path-based with full false-path elimination. The netlist reader builds exact truth tables for 347 of SKY130's 428 cells; the rest fall back to name matching.

## Radiation Hardening

`--tmr` triples every flip-flop and inserts a majority voter, the technique Voyager and most satellites since the 1970s were hardened with, except done by the synthesiser instead of by hand.

Inserting redundancy is the easy half. `--fi` proves it worked, by asking a SAT solver whether any single upset anywhere in the design can change an output. Unsatisfiable means every one is masked. Where a fault does get through, the solver names the node.

```
takahe: fi: 12 sites (12 sequential), 9 watched nets
takahe: fi: no single upset reaches an output
```

Exit status is nonzero when anything is unprotected, so it works as a CI gate. Full detail in [docs/tmr.md](docs/tmr.md).

## Beyond Binary

Takahe doesn't know what a NAND gate is. It reads a truth table from a text file and evaluates it, so synthesising in a radix other than binary needs no separate code path. The optimiser is radix-aware for the same reason: constant propagation and dead cell elimination work by evaluating truth tables rather than by rules written for any one paradigm.

Thirteen paradigms ship as `.def` files, from balanced ternary and duodecimal through to nucleotide logic, the Clifford quantum gate set, and a voting cell that is commented out because Arrow proved in 1951 it cannot exist. Adding one means writing a text file, not touching C.

See [docs/paradigms.md](docs/paradigms.md).

## Reading Netlists Back In

Synthesis goes source to gates. `--netlist` goes the other way, rebuilding a structural netlist into the same IR synthesis uses. `--seq` then recovers registers and shift chains that nothing in the netlist names, working it out twice over and checking the two answers agree.

Cell behaviour comes from the Liberty file rather than a table of known gate names, so 347 of SKY130's 428 cells build an exact truth table.

See [docs/netlists.md](docs/netlists.md).

## PLDs and Fuse Maps

`jd_read` parses JEDEC JESD3-C files, the format PLD programmers and assemblers write. It verifies both checksums, the fuse checksum and the transmission checksum, and reports a mismatch rather than failing, because a bad checksum is usually a truncated download and the fuses are still worth looking at.

Fuses only, at this point. Turning a fuse array back into equations needs the device geometry, which is the next piece.

## Older Targets

Alongside the four PDKs there's `lib/ttl7400.lib`, the 74LS series as a technology library. Area is packages rather than micrometres, since a 74LS00 puts four 2-input NANDs in one 14-pin DIP, so a NAND costs 0.25 of a chip and the area-driven mapper minimises the parts count on the board.

```bash
./takahe --lib lib/ttl7400.lib --map board.v design.sv
```

Delays are typical 74LS figures, so timing analysis gives a clock rate in the region of what the parts will actually do. Check them against whichever vendor you're buying from before trusting a report.

## Diagnostics

Unrecoverable errors produce a structured ABEND dump in the mainframe tradition, showing what the tool was doing and how full each pool was when it failed. Every optimisation pass sits behind a CICS-style transaction journal, so a pass that fails or makes things worse rolls back instead of leaving a half-optimised netlist.

Error messages are structured codes loaded from `lang/`, currently English and Te Reo Māori. Adding a language is adding a `.txt` file.

See [docs/diagnostics.md](docs/diagnostics.md).

## Project Structure

```
src/
├── main.c       CLI entry point
├── tk_abend.c   ABEND dumps + bilingual messages
├── tk_data.c    finds defs/ and lang/ relative to the binary
├── tk_jrn.c     CICS-style transaction journal
├── lex/         SV + VHDL lexers, preprocessor, .def loader
├── parse/       SV + VHDL recursive descent parsers
├── pld/         JEDEC fuse map reader
├── elab/        constant eval, elaboration, width inference
├── rtl/         RTL IR + AST-to-RTL lowering
├── opt/         constant propagation, dead cell elimination,
│                CNF encoding, CDCL SAT, fault injection
├── xform/       bit-blast, pattern matching, Espresso minimiser,
│                TMR, sequential recovery, cycle simulation
├── tech/        Liberty parser, cell defs, PCHIP, STA, timing-driven opt
├── map/         iCE40 LUT mapping, memory inference
└── emit/        BLIF, Yosys JSON, gate-level Verilog output

defs/            13 computing paradigm definitions
lang/            bilingual message catalogues (en, mi)
lib/             ttl7400.lib, the 74LS target. PDK Liberty files are
                 third-party and too large, so they are not in the repo
docs/            paradigms, TMR, netlist recovery, diagnostics
```

## Support the Takahē

This tool is named after the takahē, a flightless bird declared extinct in 1898 and rediscovered alive in New Zealand's Murchison Mountains in 1948. The population is still under 500. DOC's Takahē Recovery Programme is the reason the bird exists today.

If this project is useful to you, consider helping keep them alive.

**[Donate to the Takahē Recovery Programme](https://www.doc.govt.nz/our-work/takahe-recovery-programme/donate/)**

## License

MPL 2.0. Modify Takahe's files and share your modifications, build around it and your code stays yours.
