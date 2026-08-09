# Computing Paradigms

[← back to README](../README.md)

## Overview

Takahe does not know what a NAND gate is. It reads a truth table from a text file and evaluates it. That is the whole trick, and it is why the tool synthesises in radices other than binary without a separate code path for each one.

A paradigm is a `.def` file. Adding one means writing a text file, not touching C.

## The .def Format

Each cell declares its radix, its input and output counts, and then one line per row of its truth table:

```
# Binary AND
cell AND radix 2 inputs 2 outputs 1
  truth 0 0 -> 0
  truth 0 1 -> 0
  truth 1 0 -> 0
  truth 1 1 -> 1
end

# Ternary AND (min), balanced ternary {-1, 0, +1}
cell AND radix 3 inputs 2 outputs 1
  truth -1  0 -> -1
  truth  0  1 ->  0
  truth  1  1 ->  1
  ...
end
```

The optimiser is radix-aware. Constant propagation, identity detection and dead cell elimination all work by evaluating truth tables, not by pattern-matching rules written for any particular paradigm, so a ternary design gets the same optimisation quality as a binary one.

## The Thirteen

| File | Paradigm | Radix | Origin |
|------|----------|-------|--------|
| `cells.def` | Binary | 2 | Shannon, 1938 |
| `cells_ter.def` | Balanced ternary | 3 | Brusentsov, 1958 |
| `cells_stoch.def` | Stochastic | 2* | Gaines, 1969 |
| `cells_qc.def` | Quantum (Clifford) | 2 | Feynman, 1982 |
| `cells_doz.def` | Duodecimal | 12 | Sumer, 3000 BCE |
| `cells_dna.def` | Nucleotide | 4 | LUCA, 3.7 Gya |
| `cells_epist.def` | Epistemic | 7 | Inspired by Bochvar, 1938 |
| `cells_life.def` | Cellular automata | 2 | Conway/Wolfram |
| `cells_affect.def` | Affective | 8 | Plutchik, 1980 |
| `cells_iching.def` | I Ching trigrams | 8 | 伏羲, ~3000 BCE |
| `cells_music.def` | Music theory | 12 | Pythagoras/Bach |
| `cells_quark.def` | Particle physics | 6 | Gell-Mann, 1964 |
| `cells_arrow.def` | Impossibility | 6 | Arrow, 1951 |

\* Stochastic cells run on binary hardware with probabilistic semantics. AND becomes multiplication, MUX becomes weighted addition.

## Notes On Individual Paradigms

**Ternary**: Negation is free because you just flip every trit, no two's complement needed. The 3-way MUX selects from three inputs with one control signal. Brusentsov proved this was better in 1958 and the Soviet Union cancelled it anyway.

**Quantum**: CNOT, Toffoli, Fredkin, Hadamard, the full Clifford gate set. The classical control plane for a quantum processor synthesises alongside the binary logic so one tool handles both domains.

**DNA**: Watson-Crick complement serves as the NOT gate, the CODON cell maps three nucleotides to an amino acid index, and the MATCH cell detects base pairing. Your body runs 37 trillion instances of this cell library.

**Epistemic**: Seven values inspired by Bochvar's original three-valued logic (true, false, indeterminate), extended to include *justified* true, *believed* true, and *defeated* true. The CONSENSUS gate merges knowledge from multiple sources while the DEFEAT gate revokes warrants when counter-evidence arrives. The extension beyond Bochvar's original three values is original work.

**Duodecimal**: The Mesopotamians counted in base-12 five thousand years ago and we still use their system for hours, months, and music. The half-adder correctly computes 7 sheep + 8 sheep = 3 sheep carry 1 dozen.

**Arrow's impossibility**: A `.def` file that documents its own impossibility. The FAIR voting cell is commented out because Arrow proved in 1951 that no truth table satisfying all three fairness axioms can exist, and the file contains the proof.

**I Ching**: Closes the historical loop. Leibniz saw the trigrams in 1703 and recognised binary arithmetic, Shannon formalised it in 1938, and Takahe generalised beyond it in 2026. The I Ching was a truth table lookup three millennia before anyone called it that.

## Designs

Several of the test designs exercise non-binary paradigms directly:

| Design | Paradigm | Cells |
|--------|----------|-------|
| `designs/setun70.sv` | Ternary processor | 153 |
| `designs/dozenal_alu.sv` | Base-12 arithmetic | 41 |
| `designs/ruru.sv` | Probabilistic processor | 157 |
| `designs/qsim.sv` | Quantum gate simulator | 59 |

Synthesise one with the matching radix:

```bash
./takahe --radix 3 --opt --parse designs/setun70.sv
```
