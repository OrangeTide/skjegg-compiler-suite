program bufferedio;
{ Exercises io.buffered over the freestanding rv tier: a buffered writer batches
  all output into one write at Flush, and a buffered reader peeks a byte and
  pushes it back (UngetStr), then reads the input a line at a time (ReadLine),
  proving the whole buffering layer is Kobold userland over the raw fd seam. }
uses io.buffered;
var
  w: buffered.BufWriter;
  r: buffered.BufReader;
  line: string;
  c: char;
begin
  buffered.OpenReader(r, 0);
  buffered.OpenWriter(w, 1);

  { Peek the first byte, then push it back so ReadLine still sees the whole first
    line: the arbitrary pushback the runtime's single byte does not promise. }
  if buffered.GetChar(r, c) then
  begin
    buffered.PutStr(w, 'peek=');
    buffered.PutChar(w, c);
    buffered.PutChar(w, chr(10));
    buffered.UngetStr(r, '' + c);
  end;

  { Echo every line with a prefix.  Nothing reaches the fd until Flush, so the run
    of PutStr/PutChar above and here is one write. }
  while buffered.ReadLine(r, line) do
  begin
    buffered.PutStr(w, '> ');
    buffered.PutStr(w, line);
    buffered.PutChar(w, chr(10));
  end;
  buffered.Flush(w);
end.
