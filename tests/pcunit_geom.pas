unit pcgeom;
{ Depends on pcmathu in its interface (TPoint is used in a heading) and calls
  pcmathu.AddOne from a body, so it exercises a transitive interface dependency. }
interface
uses pcmathu;
function Area(p: TPoint): integer;
function Bumped(n: integer): integer;
implementation
function Area(p: TPoint): integer;
begin
  Area := p.x * p.y;
end;
function Bumped(n: integer): integer;
begin
  Bumped := AddOne(AddOne(n));
end;
end.
