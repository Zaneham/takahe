# Diagnostics

## ABEND Dumps

When Takahe hits an unrecoverable error it produces a structured ABEND dump in the mainframe tradition, showing what the tool was doing when it failed. The pattern comes from IBM's CICS (1968). If you are going to crash, crash informatively.

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
```

The pool figures are the useful part. A dump that says a pass ran out of nets at 41 of 100 tells you what to raise. One that only says "internal error" does not.

Under `--lang mi` the same dump reads `Wāhanga` and `Take` for the module and reason fields.

## Error Codes

All error messages use structured codes (`TK001`–`TK099`) loaded from text files in `lang/`. Messages are keyed by code rather than by line position, so catalogues can be edited freely.

| Range | Category |
|-------|----------|
| TK001–009 | Lexer and parser errors |
| TK010–019 | Elaboration (parameters, width) |
| TK020–029 | RTL lowering (pool exhaustion, no driver) |
| TK030–039 | Timing (setup/hold violations, combinational loops) |
| TK040–049 | Mapping (bit-blast overflow, Espresso limits) |
| TK090–099 | ABEND dump fields |

To add a language, create `lang/<code>.txt` with one message per line in the form:

```
TK021 no driver for net '%s'
```

A missing catalogue is not fatal. Takahe falls back to whichever language did load.

## Transaction Journaling

Every optimisation pass is protected by a CICS-style transaction journal. If a pass fails, or makes things worse, the netlist rolls back to its pre-pass state. There are no half-optimised netlists.

The journal records every cell addition, deletion and modification, and rollback replays it in reverse.

This matters most for the passes that rebuild logic cones rather than tweaking them. Espresso minimisation journals each cone individually, so one cone failing to rebuild rolls back that cone and leaves the rest of the design alone.

## Checking The Result

Two flags answer "did that pass break anything":

| Flag | What it does |
|------|--------------|
| `--equiv` | Equivalence check, pre-opt against post-opt. Tries a SAT miter first, which settles the question outright where the design is bit-level. Falls back to vectors otherwise, exhaustive at 24 input bits or fewer and 100K random above that. |
| `--hash` | 64-bit fingerprint of the synthesised netlist, for spotting when a change altered the output at all. |

See [tmr.md](tmr.md) for `--fi`, which asks a harder question about single-event upsets.
