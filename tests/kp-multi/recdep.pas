unit recdep;

interface

type Point = record
  x: integer;
  y: integer;
  tag: string;
end;

function Make(x, y: integer; tag: string): Point;
function SumXY(const p: Point): integer;

implementation

function Make(x, y: integer; tag: string): Point;
begin
  Make.x := x;
  Make.y := y;
  Make.tag := tag;
end;

function SumXY(const p: Point): integer;
begin
  SumXY := p.x + p.y;
end;

end.
