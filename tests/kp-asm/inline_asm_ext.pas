program inline_asm_ext;
{ Extended inline assembly, the simple register-constraint subset: "r" inputs,
  "=r" outputs, and "+r" inout, all integer-class operands, with GNU Intel-syntax
  templates and %N operand substitution.  Single-instruction templates keep the
  text free of embedded newlines (a Pascal string has no escapes).  x86-64 only,
  run in the aot lane like the basic fixture, since the in-process JIT does not
  assemble text (doc/languages/kobold/kp-grammar.md). }
var
    a, b, r: integer;
begin
    a := 40;
    b := 2;
    { inout operand: a := a + b }
    asm('add %0, %1' : '+r'(a) : 'r'(b));
    WriteLn('a = ', a);
    { output and input: r := a }
    asm('mov %0, %1' : '=r'(r) : 'r'(a));
    WriteLn('r = ', r);
    { an input operand may be any expression, not just a variable }
    asm('mov %0, %1' : '=r'(r) : 'r'(a + 8));
    WriteLn('r = ', r);
end.
