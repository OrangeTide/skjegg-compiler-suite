{ Test Roman numeral conversion built with ansistring concatenation }
program TestRoman;
var
  n, i: integer;
  s: string;
  symc: array[1..7] of string;
  symv: array[1..7] of integer;
begin
  symc[1] := 'M'; symv[1] := 1000;
  symc[2] := 'D'; symv[2] := 500;
  symc[3] := 'C'; symv[3] := 100;
  symc[4] := 'L'; symv[4] := 50;
  symc[5] := 'X'; symv[5] := 10;
  symc[6] := 'V'; symv[6] := 5;
  symc[7] := 'I'; symv[7] := 1;

  { Convert 37: XXXVII }
  n := 37;
  s := '';
  for i := 1 to 7 do
    while n >= symv[i] do begin
      s := s + symc[i];
      n := n - symv[i];
    end;
  writeln(s);

  { Convert 8: VIII }
  n := 8;
  s := '';
  for i := 1 to 7 do
    while n >= symv[i] do begin
      s := s + symc[i];
      n := n - symv[i];
    end;
  writeln(s);

  { Convert 3: III }
  n := 3;
  s := '';
  for i := 1 to 7 do
    while n >= symv[i] do begin
      s := s + symc[i];
      n := n - symv[i];
    end;
  writeln(s);
end.
