program dynarray3;
{ Dynamic arrays of records, the guest-heap direct-addressing slice. A record element is
  transparent guest data addressed inline: a[i].x is a direct load or store, and a whole
  record copies in and out of an element. The record mixes 4-byte and 8-byte (real) fields
  to exercise element size and alignment. Run under kprun it prints
  1 10 0.25 2 20 0.5 3 30 0.75 22 99. }
procedure emit(v: integer); external;
procedure emitf(x: real); external;

type
  Point = record x, y: integer; w: real end;

var
  a: array of Point;
  p: Point;
  i: integer;
begin
  SetLength(a, 3);
  for i := 0 to High(a) do
  begin
    a[i].x := i + 1;
    a[i].y := (i + 1) * 10;
    a[i].w := (i + 1) / 4.0;
  end;
  for i := 0 to High(a) do
  begin
    emit(a[i].x);
    emit(a[i].y);
    emitf(a[i].w);
  end;
  p := a[1];              { whole-record read from an element }
  emit(p.x + p.y);       { 22 }
  p.x := 99;
  a[2] := p;             { whole-record write into an element }
  emit(a[2].x);          { 99 }
end.
