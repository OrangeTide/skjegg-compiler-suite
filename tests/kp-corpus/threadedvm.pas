{ Token-threaded dispatch: a bytecode inner loop whose NEXT is a computed goto
  through a table of label addresses, goto disp[code[pc]].  This is the
  indirect-threaded (gforth NEXT) shape, the label-value counterpart of
  directthread.pas's function-pointer table.  The dispatch table and the labels
  are routine-local (a label value is second-class: it may live only in the
  frame of the routine that made it), so the machine runs inside a procedure. }
program threadedvm;
procedure emit(v: integer); external;

procedure run;
var
  disp: array[0..3] of label;
  code: array[0..3] of integer;
  operand: array[0..3] of integer;
  acc, pc: integer;
begin
  { build the dispatch table: one label address per opcode }
  disp[0] := @op_set;
  disp[1] := @op_add;
  disp[2] := @op_mul;
  disp[3] := @op_halt;

  { the program: set 3, add 4, mul 2, halt }
  code[0] := 0;  operand[0] := 3;
  code[1] := 1;  operand[1] := 4;
  code[2] := 2;  operand[2] := 2;
  code[3] := 3;  operand[3] := 0;

  acc := 0;
  pc := 0;
  goto disp[code[pc]];        { NEXT }

op_set:
  acc := operand[pc];
  pc := pc + 1;
  goto disp[code[pc]];

op_add:
  acc := acc + operand[pc];
  pc := pc + 1;
  goto disp[code[pc]];

op_mul:
  acc := acc * operand[pc];
  pc := pc + 1;
  goto disp[code[pc]];

op_halt:
  emit(acc)                   { (3 + 4) * 2 = 14 }
end;

begin
  run
end.
