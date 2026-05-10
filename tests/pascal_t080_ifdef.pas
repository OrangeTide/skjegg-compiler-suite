program t080_ifdef;

{$IFDEF FPC}
procedure FpcProc;
begin
  writeln('fpc');
end;
{$ELSE}
procedure FpcProc;
begin
  writeln('self');
end;
{$ENDIF}

begin
  { IFDEF false branch (FPC is not defined in this compiler) }
  {$IFDEF FPC}
  write('n');
  {$ELSE}
  write('y');
  {$ENDIF}

  { IFNDEF true branch (FPC is not defined) }
  {$IFNDEF FPC}
  write('y');
  {$ELSE}
  write('n');
  {$ENDIF}

  { IFDEF without ELSE — skipped entirely }
  {$IFDEF FPC}
  write('BUG');
  {$ENDIF}

  { IFNDEF without ELSE }
  {$IFNDEF FPC}
  write('y');
  {$ENDIF}

  writeln;

  { Nested IFNDEF }
  {$IFNDEF FPC}
    {$IFNDEF FPC}
    write('nested');
    {$ENDIF}
  {$ENDIF}
  writeln;

  { Procedure from conditional block }
  FpcProc;
end.
