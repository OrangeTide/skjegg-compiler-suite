program nest2;
type Pt = record x, y: integer; end;
procedure emitf(v: real); external;
procedure run;
var acc: real; origin: Pt;
  function scaled(k: integer): real;
  begin
    scaled := acc * k + origin.x;
  end;
begin
  acc := 1.5;
  origin.x := 4;
  emitf(scaled(3));
end;
begin
  run;
end.
