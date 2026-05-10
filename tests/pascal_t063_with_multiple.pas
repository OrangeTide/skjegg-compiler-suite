program WithMultiple;
type
  TRec1 = record
    x: integer;
  end;
  TRec2 = record
    x: integer;
    y: integer;
  end;
var
  r1: TRec1;
  r2: TRec2;
begin
  r1.x := 10;
  r2.x := 20;
  r2.y := 30;
  with r1, r2 do begin
    writeln(x);
    writeln(y);
  end
end.
