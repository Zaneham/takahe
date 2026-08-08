# Reading Netlists Back In

## Overview

Synthesis goes source to gates. `--netlist` goes the other way. Hand it a structural netlist full of standard cells and it rebuilds the design in the same IR that synthesis uses, which means every pass that works on a synthesised design also works on a recovered one.

```bash
./takahe --netlist --seq --lib sky130.lib recovered.v
```

## Recovering Sequential Structure

```
takahe: RTL: 84 nets, 79 cells
takahe: seq: 16 flops (2 held, 14 shift, 0 multi-source)
  chain 0: 8 stages, head net '\a_reg[0]'
  chain 1: 8 stages, head net '\b_reg[0]'
  group 0: 8 flops, clock 'clknet_1_1__leaf_clk'
  group 1: 8 flops, clock 'clknet_1_0__leaf_clk'
```

Nothing in that netlist says "shift register". `--seq` works it out twice over, once by walking each flop's D cone back to whichever flops feed it, and again by grouping flops that share control signals. Two 8-bit registers, agreed on by both methods.

Tracking self-reference separately from external sources is what stops an enable mux from hiding a shift register, which is the usual way this analysis goes wrong.

## Understanding Cells

Cell behaviour comes from the Liberty file rather than from a table of known gate names, so a cell like `a31o` (`X = (A1&A2&A3) | B1`) is understood rather than merely recognised. The function expression is parsed and evaluated into a truth table, which means a PDK that spells xor2 the long way round causes no trouble.

347 of SKY130's 428 cells build an exact truth table. The remainder are flops and physical fill, handled separately or not at all. Sequential cells are rejected rather than guessed at.

## Round-Tripping

Because the same tool synthesises, it can hand the design back:

```bash
./takahe --netlist --lib sky130.lib --map out.v recovered.v
```

Read, emit, and re-read gives the same net count, cell count and register structure, which is a decent check that nothing was lost in the middle.

## Simulating What Was Recovered

Reverse engineering a circuit and never running it is a bit like reading a recipe and calling it dinner. The cycle simulator drives a recovered netlist directly: set the inputs, settle the logic, tick the clock, read the outputs.

It is two-valued and zero delay. Combinational logic settles by iterating to a fixed point rather than by levelising, which is slower and much harder to get wrong, and a netlist that will not settle is reported rather than quietly truncated, because a combinational loop is something you want to know about.

All D inputs are sampled before any Q moves, so a shift register shifts by one rather than racing to the end.
