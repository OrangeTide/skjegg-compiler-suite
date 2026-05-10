{ Test Roman numeral conversion - simple }
program TestRoman;
var
  n, i, len: integer;
  s: string[20];
  symc: array[1..7] of char;
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
  len := 0;
  for i := 1 to 7 do begin
    while n >= symv[i] do begin
      len := len + 1;
      s[len] := symc[i];
      n := n - symv[i];
    end;
  end;
  s[0] := chr(len);
  writeln(s);

  { Convert 8: VIII }
  n := 8;
  s := '';
  len := 0;
  for i := 1 to 7 do begin
    while n >= symv[i] do begin
      len := len + 1;
      s[len] := symc[i];
      n := n - symv[i];
    end;
  end;
  s[0] := chr(len);
  writeln(s);

  { Convert 3: III }
  n := 3;
  s := '';
  len := 0;
  for i := 1 to 7 do begin
    while n >= symv[i] do begin
      len := len + 1;
      s[len] := symc[i];
      n := n - symv[i];
    end;
  end;
  s[0] := chr(len);
  writeln(s);
end.
