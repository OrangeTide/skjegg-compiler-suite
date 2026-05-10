{ Test functions with multiple return paths }
program TestMultiReturn;

function classify(n: integer): integer;
begin
  if n > 0 then begin
    classify := 1;
    exit;
  end;
  if n < 0 then begin
    classify := -1;
    exit;
  end;
  classify := 0;
end;

function clamp(n, lo, hi: integer): integer;
begin
  if n < lo then begin
    clamp := lo;
    exit;
  end;
  if n > hi then begin
    clamp := hi;
    exit;
  end;
  clamp := n;
end;

begin
  writeln(classify(42));
  writeln(classify(-7));
  writeln(classify(0));
  writeln(clamp(5, 1, 10));
  writeln(clamp(-3, 0, 100));
  writeln(clamp(999, 0, 100));
end.
