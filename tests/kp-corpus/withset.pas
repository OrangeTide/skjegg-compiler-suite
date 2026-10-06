program withset;
type Rec = record x: integer; f: set of 0..7; end;
var r: Rec; a, b: set of 0..7; ok: boolean;
procedure emit(v: integer); external;
begin
  with r do begin
    x := 42;
    f := [1, 2];
  end;
  a := [1, 2];
  b := [1, 2, 3];
  ok := (a < b) and not (a < a);
  if ok then emit(r.x);
end.
