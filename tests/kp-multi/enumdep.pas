unit enumdep;

interface

type Direction = (North, East, South, West);

const Ready = true;

function Opposite(d: Direction): Direction;
function Facing(d: Direction): boolean;
procedure Advance(var steps: integer);

implementation

function Opposite(d: Direction): Direction;
begin
  case d of
    North: Opposite := South;
    South: Opposite := North;
    East:  Opposite := West;
    West:  Opposite := East;
  end;
end;

function Facing(d: Direction): boolean;
begin
  Facing := d = North;
end;

procedure Advance(var steps: integer);
begin
  steps := steps + 1;
end;

end.
