unit rxfacade;
{ an umbrella (facade) unit: it `uses rxbase` and re-exports the whole
  dependency, so a consumer that `uses rxfacade` reaches rxbase's Vec, MakeVec,
  and SumVec through the facade with no `uses rxbase` of its own. It also adds a
  routine of its own that takes and returns the re-exported type. }
uses rxbase;

interface

reexport rxbase;
function Doubled(const v: rxbase.Vec): rxbase.Vec;

implementation

function Doubled(const v: rxbase.Vec): rxbase.Vec;
begin
  Doubled.x := v.x * 2;
  Doubled.y := v.y * 2;
end;

end.
