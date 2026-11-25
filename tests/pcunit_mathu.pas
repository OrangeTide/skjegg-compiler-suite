unit pcmathu;
{ A leaf unit: an exported const, a record type, a function, and a procedure. }
interface
const
  Answer = 42;
type
  TPoint = record
    x, y: integer;
  end;
function AddOne(n: integer): integer;
function MakePoint(ax, ay: integer): TPoint;
procedure Greet;
implementation
function AddOne(n: integer): integer;
begin
  AddOne := n + 1;
end;
function MakePoint(ax, ay: integer): TPoint;
begin
  MakePoint.x := ax;
  MakePoint.y := ay;
end;
procedure Greet;
begin
  writeln('hello from pcmathu');
end;
end.
