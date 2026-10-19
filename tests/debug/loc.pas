{ Fixture for test-kp-rv-loc: debug-info line markers (-g) on the Kobold path.
  Each statement sits on its own line so kpcom emits one LOC per line and the
  RISC-V backend records a "# line N" marker.  The test only inspects the
  assembly, so the program need not do anything interesting. }
program loc;
var
    x: integer;

function add(a: integer; b: integer): integer;
var
    s: integer;
begin
    s := a + b;
    add := s;
end;

begin
    x := add(2, 3);
    WriteLn(x);
end.
