program enumindex;
{ A named ordinal type as an array index: array[TColor] rather than array[Lo..Hi]. The index
  spans the whole range of the named type, an enum or a named subrange, for a variable, a
  constant array, and a for-loop index. Run under kprun it prints 10 20 30 100 50 1 4 9. }
procedure emit(v: integer); external;

type
  Color = (Red, Green, Blue);
  Digit = 1..3;

const
  rgb: array[Color] of integer = (200, 100, 50);

var
  t: array[Color] of integer;
  d: array[Digit] of integer;
  c: Color;
  i: integer;
begin
  t[Red] := 10;
  t[Green] := 20;
  t[Blue] := 30;
  for c := Red to Blue do emit(t[c]);   { 10 20 30 }

  emit(rgb[Green]);                       { 100 }
  emit(rgb[Blue]);                        { 50 }

  for i := 1 to 3 do d[i] := i * i;
  for i := 1 to 3 do emit(d[i]);          { 1 4 9 }
end.
