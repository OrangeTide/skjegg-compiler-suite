{ Fixture for test-pc-loc: debug-info line markers (-g) on the Compact Pascal
  path.  Each statement sits on its own line so skj-pc emits one IR_LOC per line
  and the ColdFire backend records a "| line N" comment.  The test only inspects
  the assembly, so the program need not do anything interesting. }
program loc;
var
    x: integer;

function add(a, b: integer): integer;
begin
    add := a + b;
end;

begin
    x := add(2, 3);
    x := x + 1;
end.
