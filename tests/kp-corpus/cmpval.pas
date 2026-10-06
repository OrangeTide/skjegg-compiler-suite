program cmpval;
{ A comparison used as an integer value is a 0/1 boolean, matching true = 1 and
  Ord(true) = 1, not a negated -1/0. Boolean and, or, and not compose on 0/1.
  Run under kprun it prints 1 0 1 0 1 1 0 1 1. }
procedure emit(v: integer); external;

var
  i, j: integer;
  b: boolean;

begin
  i := 5; j := 9;
  emit(Ord(i = 5));                { 1 }
  emit(Ord(i = 6));                { 0 }
  emit(Ord(i < j));                { 1 }
  emit(Ord(i > j));                { 0 }
  emit(Ord(i <> j));               { 1 }
  b := i = 5;
  emit(Ord(b));                    { 1 }
  emit(Ord(not b));                { 0 }
  emit(Ord((i = 5) and (j = 9)));  { 1 }
  emit(Ord((i = 6) or (j = 9)));   { 1 }
end.
