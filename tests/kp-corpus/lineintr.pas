program lineintr;
{ __line__ is a compile-time intrinsic: an integer constant equal to the source line of its
  own occurrence. A diagnostic helper takes it as an argument, so the caller marks the failing
  line without counting by hand and the helper reports the call site, not its own body. It is
  an ordinary integer, usable in arithmetic. Run under kprun it prints 14 16 17. }
procedure emit(v: integer); external;

procedure note(where: integer);
begin
  emit(where);           { echoes the caller's line, not this line }
end;

begin
  note(__line__);        { the call site is line 14 }
  emit(
    __line__);           { the intrinsic is on line 16 }
  emit(__line__ + 0);    { usable in an expression, line 17 }
end.
