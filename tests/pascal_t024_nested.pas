program t024_nested;
var result: integer;

procedure Outer;
var a: integer;

  procedure Inner;
  var b: integer;
  begin
    b := 10;
    a := a + b;
    result := a;
  end;

begin
  a := 32;
  Inner;
end;

procedure Accumulate(n: integer);
var local_n: integer;

  procedure AddToTotal;
  begin
    result := result + local_n;
  end;

begin
  local_n := n;
  if n > 0 then begin
    AddToTotal;
    Accumulate(n - 1);
  end;
end;

begin
  result := 0;
  Outer;
  writeln(result);
  result := 0;
  Accumulate(5);
  writeln(result);
end.
