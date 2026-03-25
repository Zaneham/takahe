Triple Modular Redundancy in Takahe
====================================

Overview
--------
The --tmr flag automatically hardens a design against single-event
upsets (SEUs) caused by radiation. Every flip-flop is tripled and a
majority voter is inserted on each output. If one copy gets flipped
by a charged particle, the other two outvote it.

This is the same technique used on the Voyager spacecraft, the Space
Shuttle flight computers, and most satellites built since the 1970s.
The difference is that those designs were hardened by hand. Takahe
does it automatically.

I originally learnt about TMR while building an emulator for the
Voyager Flight Data Subsystem from JPL documentation I found at
Wichita State University.
The FDS uses triple-redundant voting on its critical registers and
it struck me that a synthesiser could do this automatically instead
of making the designer wire up every voter by hand. So here we are.


How It Works
------------
Given a DFF with data input D, clock C, and output Q:

  Before TMR:
    D ---> [DFF] ---> Q ---> (downstream logic)

  After TMR:
    D ---> [DFF_A] ---> Q_A ---\
    D ---> [DFF_B] ---> Q_B --->  [VOTER] ---> Q ---> (downstream)
    D ---> [DFF_C] ---> Q_C ---/

The voter computes: Q = (Q_A & Q_B) | (Q_B & Q_C) | (Q_A & Q_C)

All three DFFs receive the same inputs. The voter output drives the
original net so no downstream rewiring is needed.


Voter Implementation
--------------------
The voter decomposes to three AND gates and two OR gates:

    t0 = AND(Q_A, Q_B)
    t1 = AND(Q_B, Q_C)
    t2 = AND(Q_A, Q_C)
    t3 = OR(t0, t1)
    Q  = OR(t3, t2)

No new cell types are introduced. The voter gates are standard
combinational logic and flow through the rest of the pipeline
(optimisation, bit-blast, technology mapping, FPGA, emitters)
without any special handling.


Two Modes
---------
  --tmr       Sequential only. Triplicates DFF and DFFR cells.
              Combinational logic is shared between replicas.
              This is the standard approach for most applications.

  --tmr-full  Full TMR. Triplicates every cell and inserts voters
              on all outputs. Protects against single-event
              transients (SETs) in combinational logic too.
              Roughly 3x area overhead.


Overhead
--------
Per DFF triplicated:
  +2 DFF cells (replicas B and C)
  +3 AND cells (voter)
  +2 OR cells (voter)
  +8 nets (3 replica outputs + 5 voter intermediates)

Example: Voyager FDS
  Before: 67 cells (15 DFFs)
  After:  172 cells (45 DFFs + 15 voters)

Example: Minuteman D17B
  Before: 43 cells (11 DFFs)
  After:  120 cells (33 DFFs + 11 voters)


Pipeline Placement
------------------
TMR runs after optimisation and before bit-blast/mapping. This
means the optimiser works on the smaller original netlist, and
the voter gates get mapped to library cells or LUTs along with
everything else.


Limitations
-----------
- TMR does not protect against multiple simultaneous upsets in
  the same voter domain. For that you need temporal redundancy
  or scrubbing.

- The voter itself is not hardened. For extreme environments,
  consider using radiation-hard cell libraries (e.g. RHBD) in
  addition to TMR.

- Clock and reset networks are shared between replicas. If the
  clock tree is upset, all three replicas may latch the wrong
  value simultaneously.

- TMR increases power consumption roughly 3x for the sequential
  elements. This matters for battery-powered space missions.
