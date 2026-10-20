program readeof;
{ echoes stdin verbatim, reading one character at a time until end of input (the
  sticky latch io_eof).  io_eof is reached through the FFI for now (it is guest
  runtime; the Kobold Eof builtin that would call it is a later addition).  The
  input's final line has no trailing newline, so the test also covers that the
  last character is delivered before the latch trips.  Exercises the freestanding
  console latch over the read seam. }
function io_eof: Boolean; external cdecl;
var
  ch: char;
begin
  while not io_eof do begin
    Read(ch);
    Write(ch);
  end;
end.
