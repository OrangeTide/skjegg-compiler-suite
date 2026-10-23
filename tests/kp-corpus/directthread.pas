{ Direct-threaded dispatch: an array of routine values (op handlers) walked and
  called through an array element, table[code[pc]](acc, operand[pc]).  This is
  the function-pointer threaded-code shape; it exercises an indirect call whose
  callee is a computed designator (an array element), not a bare name. }
program directthread;
procedure emit(v: integer); external;

function h_set(acc, x: integer): integer;
begin
  h_set := x;
end;
function h_add(acc, x: integer): integer;
begin
  h_add := acc + x;
end;
function h_mul(acc, x: integer): integer;
begin
  h_mul := acc * x;
end;

const
  OP_SET = 0;
  OP_ADD = 1;
  OP_MUL = 2;
  N = 3;

var
  table: array[0..2] of function (acc, x: integer): integer;
  code: array[0..2] of integer;
  operand: array[0..2] of integer;
  acc, pc: integer;
begin
  table[OP_SET] := h_set;
  table[OP_ADD] := h_add;
  table[OP_MUL] := h_mul;

  code[0] := OP_SET;  operand[0] := 3;
  code[1] := OP_ADD;  operand[1] := 4;
  code[2] := OP_MUL;  operand[2] := 2;

  acc := 0;
  for pc := 0 to N - 1 do
    acc := table[code[pc]](acc, operand[pc]);

  emit(acc);   { (3 + 4) * 2 = 14 }
end.
