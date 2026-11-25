program pascal_unit_use;
{ Uses pcgeom (which transitively pulls pcmathu) and pcmathu directly: a diamond
  that must inject pcmathu once.  Exercises an exported const, record type,
  function, procedure, and a transitive-dependency call. }
uses pcgeom, pcmathu;
var
  p: TPoint;
begin
  p := MakePoint(3, 4);
  writeln('answer=', Answer);
  writeln('addone=', AddOne(Answer));
  writeln('area=', Area(p));
  writeln('bumped=', Bumped(10));
  Greet;
end.
