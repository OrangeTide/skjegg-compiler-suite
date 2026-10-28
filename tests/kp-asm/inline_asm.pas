program inline_asm;
{ Basic inline assembly: asm('text') emits the one string literal verbatim into
  the generated assembly.  The payload here is a C-style block comment, which
  every target assembler accepts, so the one fixture builds on each AOT backend.
  A live value spans the asm to confirm it does not perturb code generation.
  AOT only: the in-process JIT does not assemble text, so this fixture runs in
  the aot lane (doc/languages/kobold/kp-grammar.md). }
var
    a: integer;
begin
    a := 40;
    asm('/* kobold inline asm marker */');
    WriteLn('a = ', a + 2);
end.
