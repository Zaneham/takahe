// Each output is named after the 74LS part that should end up driving it,
// so a mismapping reads as a wrong label rather than a diff.
module parts(input a, input b,
             output nand_here, output nor_here,
             output xor_here,  output inv_here);
  assign nand_here = ~(a & b);
  assign nor_here  = ~(a | b);
  assign xor_here  = a ^ b;
  assign inv_here  = ~a;
endmodule
