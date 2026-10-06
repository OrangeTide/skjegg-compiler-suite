program constarray;
{ Typed constant arrays with ordinal elements, the constant-aggregate foundation. A constant
  array is emitted as a read-only global image (the elements packed little-endian) and indexed
  through the ordinary fixed-array path, whether it is a program-level or a routine-local
  const. Run under kprun it prints 1 4 9 16 25 7 9 1 2. }
procedure emit(v: integer); external;

const
  squares: array[1..5] of integer = (1, 4, 9, 16, 25);
  flags: array[0..2] of boolean = (true, false, true);

function pick(i: integer): integer;
const
  local: array[0..2] of integer = (7, 8, 9);
begin
  pick := local[i];
end;

var
  i: integer;
begin
  for i := 1 to 5 do emit(squares[i]);   { 1 4 9 16 25 }
  emit(pick(0));                          { 7 }
  emit(pick(2));                          { 9 }
  if flags[0] then emit(1);               { 1 }
  if not flags[1] then emit(2);           { 2 }
end.
