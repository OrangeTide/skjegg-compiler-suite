{ Enumerated types as a registered ordinal type (P4): an enum variable and
  record field, Ord/Succ/Pred and High/Low on the type, set of an enum with
  membership, case over an enum, an array indexed by the enum, and an enum
  control variable in a for loop. }
program EnumTypes;
type
  TColor = (cRed, cGreen, cBlue, cWhite);
  TColorSet = set of TColor;
  TRec = record
    c: TColor;
    n: integer;
  end;
  TPalette = array[TColor] of string;
var
  x: TColor;
  r: TRec;
  s: TColorSet;
  pal: TPalette;
  c: TColor;
begin
  x := cGreen;
  writeln(Ord(x), ' ', Ord(Succ(x)), ' ', Ord(Pred(x)));
  writeln(Ord(Low(TColor)), ' ', Ord(High(TColor)));
  r.c := cBlue;
  r.n := 7;
  writeln(Ord(r.c), ' ', r.n);
  s := [cRed, cBlue, cWhite];
  if cBlue in s then
    writeln('blue-in');
  if not (cGreen in s) then
    writeln('no-green');
  case x of
    cRed:   writeln('red');
    cGreen: writeln('green');
    cBlue:  writeln('blue');
    cWhite: writeln('white');
  end;
  pal[cRed] := 'R';
  pal[cGreen] := 'G';
  pal[cBlue] := 'B';
  pal[cWhite] := 'W';
  for c := Low(TColor) to High(TColor) do
    write(pal[c]);
  writeln;
end.
