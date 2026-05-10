{ Test fillchar builtin }
program TestFillchar;
var
  a: array[1..5] of integer;
  s: string[20];
  i: integer;
begin
  { fill integer array with zeros }
  for i := 1 to 5 do
    a[i] := 99;
  fillchar(a, 5 * 4, 0);
  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;

  { zero out a string }
  s := 'Hello';
  writeln(length(s));
  fillchar(s, 21, 0);
  writeln(length(s));
end.
