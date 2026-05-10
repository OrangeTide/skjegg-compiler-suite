program t051_inc_dec;

var
  x: integer;

procedure test_var(var v: integer);
begin
  inc(v);
  writeln(v);
  dec(v, 3);
  writeln(v);
end;

begin
  { Basic inc }
  x := 10;
  inc(x);
  writeln(x);      { 11 }

  { Basic dec }
  dec(x);
  writeln(x);      { 10 }

  { Inc with amount }
  inc(x, 5);
  writeln(x);      { 15 }

  { Dec with amount }
  dec(x, 7);
  writeln(x);      { 8 }

  { Inc/dec with var param }
  x := 20;
  test_var(x);     { 21, then 18 }
  writeln(x);      { 18 - verify passed by reference }

  { Edge cases }
  x := 0;
  dec(x);
  writeln(x);      { -1 }
  inc(x);
  writeln(x);      { 0 }
end.
