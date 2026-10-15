program conf_trap_mapkey;
{ Conformance: reading a map at an absent key with m[k] traps and exits 70; the
  trap-free read is TryGet (kp-builtins.md, kp-evaluation.md). The map holds one
  key so it is a live, allocated map, and a different, absent key is read. The
  trap fires before emit runs, so stdout is empty. }
procedure emit(v: integer); external;

var
  m: map of integer;
begin
  m['a'] := 1;
  emit(m['b']);
end.
