program mu_enum;
{ an enum type exported from a unit: its members, a function over it, and a
  var parameter mutated across the boundary. }
uses enumdep as e;
var
  steps: integer;
  d: e.Direction;
begin
  d := e.Opposite(e.North);
  WriteLn('Opposite(North)=South? ', d = e.South);
  WriteLn('Facing(North)? ', e.Facing(e.North));
  WriteLn('Facing(South)? ', e.Facing(e.South));
  WriteLn('Ready? ', e.Ready);
  steps := 0;
  e.Advance(steps);
  e.Advance(steps);
  WriteLn('steps=', steps);
end.
