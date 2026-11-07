program extalias;
{ External name alias: a host import whose Kobold name differs from the C symbol
  it binds to. `Emit` is reached by that name in source, but is imported and
  linked as the runtime console symbol `io_write_str` (fd, string). }
procedure Emit(fd: integer; s: string); external cdecl name 'io_write_str';
begin
  Emit(1, 'external alias ok');
  WriteLn;
end.
