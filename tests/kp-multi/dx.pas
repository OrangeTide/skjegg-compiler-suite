unit dx;
{ an umbrella that re-exports a dotted-name dependency: `reexport dx.base`
  promotes dx.base's Scale and Half into dx's own interface, so a consumer that
  `uses dx` reaches them without naming dx.base. Exercises a dotted unit name in
  a reexport clause. }
uses dx.base;

interface

reexport dx.base;

implementation

end.
