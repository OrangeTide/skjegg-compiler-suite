unit enumuse;
uses enumdep as e;

interface

function Go: boolean;

implementation

var moves: integer;

function Go: boolean;
var d: e.Direction;
begin
  d := e.Opposite(e.North);
  e.Advance(moves);
  Go := e.Facing(d) and e.Ready;
end;

end.
