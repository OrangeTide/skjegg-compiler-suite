program stackargs;
{ Regression: arguments past the sixth integer register and the eighth float
  register are passed on the stack (SysV). s10 has four integer stack args,
  f10 has two double stack args, and fs9 has one single-precision stack arg
  (exercising the movss stack slot). Run under kprun it prints 55 55 511. }
procedure emit(v: integer); external;
procedure emitf(v: real); external;

function s10(a, b, c, d, e, f, g, h, i, j: integer): integer;
begin
  s10 := a + b + c + d + e + f + g + h + i + j;
end;

function f10(a, b, c, d, e, f, g, h, i, j: real): real;
begin
  f10 := a + b + c + d + e + f + g + h + i + j;
end;

function fs9(a, b, c, d, e, f, g, h, i: single): single;
begin
  fs9 := a + b + c + d + e + f + g + h + i;
end;

begin
  emit(s10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  emitf(f10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  emitf(fs9(1, 2, 4, 8, 16, 32, 64, 128, 256));
end.
