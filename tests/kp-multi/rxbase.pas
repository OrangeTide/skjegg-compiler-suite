unit rxbase;
{ a base unit whose names a facade re-exports. }

interface

type Vec = record
  x: integer;
  y: integer;
end;

function MakeVec(x, y: integer): Vec;
function SumVec(const v: Vec): integer;

implementation

function MakeVec(x, y: integer): Vec;
begin
  MakeVec.x := x;
  MakeVec.y := y;
end;

function SumVec(const v: Vec): integer;
begin
  SumVec := v.x + v.y;
end;

end.
