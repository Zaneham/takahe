# References

Papers and standards the implementation follows. Source files name the
algorithm and point here rather than carrying a bibliography.

## Logic minimisation

`src/xform/tk_espresso.c`, `src/xform/tk_espro.c`

Brayton, R. K., Hachtel, G. D., McMullen, C. T., & Sangiovanni-Vincentelli,
A. L. (1984). *Logic minimization algorithms for VLSI synthesis.* Kluwer
Academic Publishers. https://doi.org/10.1007/978-1-4613-2821-6

Rudell, R. L., & Sangiovanni-Vincentelli, A. L. (1987). Multiple-valued
minimization for PLA optimization. *IEEE Transactions on Computer-Aided
Design of Integrated Circuits and Systems, 6*(5), 727–750.
https://doi.org/10.1109/TCAD.1987.1270318

McGeer, P. C., Sanghavi, J. V., Brayton, R. K., & Sangiovanni-Vincentelli,
A. L. (1993). ESPRESSO-SIGNATURE: A new exact minimizer for logic functions.
*IEEE Transactions on VLSI Systems, 1*(4), 432–440.

## Transaction journalling

`src/tk_jrn.c`

IBM Corporation. (1977). *CICS/VS System Programmer's Reference Manual*
(SC33-0068). IBM Systems Library.

IBM Corporation. (2014). *CICS Transaction Server for z/OS: Recovery and
Restart Guide* (SC34-7012). IBM Knowledge Center.

Gray, J., & Reuter, A. (1993). *Transaction Processing: Concepts and
Techniques.* Morgan Kaufmann. https://doi.org/10.1016/C2009-0-27825-8

## Monotone interpolation

`src/tech/tk_pchip.c`

Fritsch, F. N., & Carlson, R. E. (1980). Monotone piecewise cubic
interpolation. *SIAM Journal on Numerical Analysis, 17*(2), 238–246.
https://doi.org/10.1137/0717021

Fritsch, F. N., & Butland, J. (1984). A method for constructing local
monotone piecewise cubic interpolants. *SIAM Journal on Scientific and
Statistical Computing, 5*(2), 300–304. https://doi.org/10.1137/0905021

Brodlie, K. W. (1980). A review of methods for curve and function drawing.
In K. W. Brodlie (Ed.), *Mathematical methods in computer graphics and
design* (pp. 1–37). Academic Press.

Fritsch, F. N. (1982). *PCHIP: Piecewise Cubic Hermite Interpolation
Package* [Computer software]. Lawrence Livermore National Laboratory.
SLATEC Common Mathematical Library, Version 4.1.

Hambly, Z. (2026). *SLATEC-Modern* [Computer software].
https://github.com/Zaneham/slatec-modern

## Static timing analysis

`src/tech/tk_sta.c`, `src/tech/tk_tdopt.c`

Hitchcock, R. B., Smith, G. L., & Cheng, D. D. (1982). Timing analysis of
computer hardware. *IBM Journal of Research and Development, 26*(1),
100–105. https://doi.org/10.1147/rd.261.0100

Sapatnekar, S. S. (2004). *Timing.* Springer.
https://doi.org/10.1007/978-0-387-21830-1

Bhasker, J., & Chadha, R. (2009). *Static timing analysis for nanometer
designs: A practical approach.* Springer.
https://doi.org/10.1007/978-0-387-93820-2

## Satisfiability

`src/opt/tk_sat.c`, `src/opt/tk_cnf.c`

Tseitin, G. S. (1968). On the complexity of derivation in propositional
calculus. *Studies in Constructive Mathematics and Mathematical Logic,
Part II*, 115–125.

Moskewicz, M. W., Madigan, C. F., Zhao, Y., Zhang, L., & Malik, S. (2001).
Chaff: Engineering an efficient SAT solver. *Proceedings of the 38th Design
Automation Conference*, 530–535. https://doi.org/10.1145/378239.379017

Eén, N., & Sörensson, N. (2003). An extensible SAT-solver. *Theory and
Applications of Satisfiability Testing (SAT 2003)*, 502–518.
https://doi.org/10.1007/978-3-540-24605-3_37

Luby, M., Sinclair, A., & Zuckerman, D. (1993). Optimal speedup of Las
Vegas algorithms. *Information Processing Letters, 47*(4), 173–180.
https://doi.org/10.1016/0020-0190(93)90029-9

## File formats

Berkeley Logic Interchange Format (BLIF). *Berkeley Logic Synthesis and
Verification Group.* `src/emit/tk_blif.c`

JEDEC. *JESD3-C, Standard Data Transfer Format Between Data Preparation
System and Programmable Logic Device Programmer.* `src/pld/jd_read.c`

Open Source Liberty. (2017). *Liberty Technical Reference Manual.* Si2
(Silicon Integration Initiative). `src/tech/tk_lib.c`, `src/tech/tk_lfn.c`

Data I/O. (1995). *Synario ABEL-HDL Reference.* `src/lex/ab_lex.c`,
`src/parse/ab_parse.c`

YosysHQ. *Yosys manual*, Appendix C (JSON netlist format).
`src/emit/tk_yosys.c`

YosysHQ. *nextpnr JSON format.* https://github.com/YosysHQ/nextpnr
`src/map/tk_fpga.c`

## Standards

IEEE. (2017). *IEEE Standard for SystemVerilog* (IEEE 1800-2017).

IEEE. (2008). *IEEE Standard VHDL Language Reference Manual* (IEEE
1076-2008).
