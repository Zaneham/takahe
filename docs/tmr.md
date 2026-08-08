# Triple Modular Redundancy

## Overview

The `--tmr` flag automatically hardens a design against single-event upsets (SEUs) caused by radiation. Every flip-flop is tripled and a majority voter is inserted on each output. If one copy gets flipped by a charged particle, the other two outvote it.

This is the same technique used on the Voyager spacecraft, the Space Shuttle flight computers, and most satellites built since the 1970s. The difference is that those designs were hardened by hand. Takahe does it automatically.

I originally learnt about TMR while building an emulator for the Voyager Flight Data Subsystem from JPL documentation I found at Wichita State University. The FDS uses triple-redundant voting on its critical registers and it struck me that a synthesiser could do this automatically instead of making the designer wire up every voter by hand. So here we are.

## How It Works

Given a DFF with data input D, clock C, and output Q:

```
Before TMR:
  D ---> [DFF] ---> Q ---> (downstream logic)

After TMR:
  D ---> [DFF_A] ---> Q_A ---\
  D ---> [DFF_B] ---> Q_B --->  [VOTER] ---> Q ---> (downstream)
  D ---> [DFF_C] ---> Q_C ---/
```

The voter computes `Q = (Q_A & Q_B) | (Q_B & Q_C) | (Q_A & Q_C)`.

All three DFFs receive the same inputs. The voter output drives the original net so no downstream rewiring is needed.

## Voter Implementation

The voter decomposes to three AND gates and two OR gates:

```
t0 = AND(Q_A, Q_B)
t1 = AND(Q_B, Q_C)
t2 = AND(Q_A, Q_C)
t3 = OR(t0, t1)
Q  = OR(t3, t2)
```

No new cell types are introduced. The voter gates are standard combinational logic and flow through the rest of the pipeline (optimisation, bit-blast, technology mapping, FPGA, emitters) without any special handling.

## Two Modes

| Flag | Scope |
|------|-------|
| `--tmr` | Sequential only. Triplicates DFF and DFFR cells, combinational logic shared between replicas. The standard approach for most applications. |
| `--tmr-full` | Triplicates every cell and inserts voters on all outputs, so single-event transients in combinational logic are covered too. Roughly 3x area overhead. |

## Overhead

Per DFF triplicated:

```
+2 DFF cells (replicas B and C)
+3 AND cells (voter)
+2 OR cells (voter)
+8 nets (3 replica outputs + 5 voter intermediates)
```

| Design | Before | After |
|--------|--------|-------|
| Voyager FDS | 67 cells (15 DFFs) | 172 cells (45 DFFs + 15 voters) |
| Minuteman D17B | 43 cells (11 DFFs) | 120 cells (33 DFFs + 11 voters) |

## Pipeline Placement

TMR runs after optimisation and before bit-blast/mapping. This means the optimiser works on the smaller original netlist, and the voter gates get mapped to library cells or LUTs along with everything else.

## Verifying It Worked

Inserting redundancy is the easy half. Nothing about `--tmr` guarantees the result is actually hardened, and the classic way it fails is that an optimiser looks at three identical logic cones, correctly concludes they compute the same thing, and merges them. Area stays at 3x, the voter still looks like a voter, immunity is gone, and nothing in the netlist says so.

`--fi` settles it:

```bash
./takahe --tmr --fi design.sv
```

```
takahe: fi: 12 sites (12 sequential), 9 watched nets
takahe: fi: 8 replica flops assumed to track their peers
takahe: fi: no single upset reaches an output
```

Two copies of the combinational logic are built against shared inputs, an XOR is spliced into one node of one copy, exactly one splice is constrained live, and a SAT solver is asked whether any input tells the copies apart. Unsatisfiable means every single upset is masked. That is a proof, not a fault campaign that ran out of vectors and stopped.

When a fault does get through, the model names the node. `--fi` blocks that one and asks again, so the answer arrives as a list rather than a single example, and a hardened design costs one solve because the very first answer is unsatisfiable.

| Flag | Fault sites |
|------|-------------|
| `--fi` | Flop outputs only, which is the stored-upset question and what TMR exists to survive. |
| `--fi-comb` | Every gate output too, so single-event transients are covered as well. Reports a great deal more, because nothing masks a glitch on the last gate before a flop. |

Exit status is nonzero when anything is unprotected, so this works as a CI gate rather than a report somebody has to read.

## How The Question Is Framed

The design is cut at the flops in the usual way, so every flop output becomes a free input and every flop D joins the real outputs as something worth watching. That keeps the whole question combinational and avoids unrolling.

Cutting has one consequence worth understanding. It hands the solver an arbitrary starting state, so left alone the solver will start the three replicas of a TMR'd flop already disagreeing, walk the vote straight past the voter, and call perfectly good hardening broken. No voted design could ever be proved that way.

The fix is an invariant rather than a favour to the checker. Flops that share a D net, a type and a reset hold the same value in every state reachable from reset, by induction on cycles with reset as the base case, so assuming they agree is sound. `--fi` reports how many flops it grouped this way, and the grouping is by shared D net rather than by cone shape, which needs no proof obligation of its own and covers everything `tm_tmr` emits, since every replica reads the original D.

The flip side is that a reported fault may need a state the design cannot actually reach. That errs the safe way, because a clean run still means clean.

## Limitations

- TMR does not protect against multiple simultaneous upsets in the same voter domain. For that you need temporal redundancy or scrubbing.

- The voter itself is not hardened. For extreme environments, consider using radiation-hard cell libraries (e.g. RHBD) in addition to TMR. `--fi-comb` will show you this directly, since the voter output appears as an unprotected site.

- Clock and reset networks are shared between replicas. If the clock tree is upset, all three replicas may latch the wrong value simultaneously.

- TMR increases power consumption roughly 3x for the sequential elements. This matters for battery-powered space missions.

- `--fi` refuses any design containing a variable shift or a multiplier. Wide operators are bit-blasted first, on a private copy so the design you go on to emit is untouched, but the bit-blaster does not yet handle SHL, SHR, SHRA, MUL or PMUX. It names the cell type it choked on rather than failing silently.

- Replica grouping recognises flops that share a D net. Hardening done by another tool, where the replicas have structurally distinct but functionally identical D cones, will not be grouped and will not be proved.
