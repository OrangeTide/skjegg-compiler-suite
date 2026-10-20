{ Fixture for test-kp-rv-trap-backtrace: a nested call chain whose deepest
  function fails an assert.  Under skj-run the Kobold runtime's fatal path asks
  the host for a symbolized backtrace (the same RV_SYS_FATAL ecall C's abort
  uses), so the stack should name deep, mid, and the program entry, then exit
  70.  Each call sits on its own line so the -g line markers are distinct. }
program ktrap;

function deep(x: integer): integer;
begin
    assert(x = 0);
    deep := x;
end;

function mid(x: integer): integer;
begin
    mid := deep(x) + 1;
end;

begin
    WriteLn(mid(1));
end.
