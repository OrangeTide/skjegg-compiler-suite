program textfilert;
{ The classic ISO/Turbo Pascal text-file verbs through io.textfile: Assign then
  Rewrite to write three lines (one built from a Write without a newline plus a
  WriteLn), then Assign + Reset and read the first line character by character
  with Read until Eoln, and the rest with a `while not Eof` ReadLn loop.  A
  missing file then shows Reset failing (IOOk false) and Eof reporting true
  straight away.  The verbs are reached through the `textfile` qualifier because
  Read/Write/ReadLn/WriteLn also name console builtins. }
uses io.textfile;
var
  f: textfile.TextFile;
  line, s: string;
  c: char;
  n: integer;
begin
  textfile.Assign(f, ParamStr(1));
  textfile.Rewrite(f);
  if textfile.IOOk(f) then
  begin
    textfile.WriteLn(f, 'alpha');
    textfile.WriteLn(f, 'beta');
    textfile.Write(f, 'gam');
    textfile.WriteLn(f, 'ma');
    textfile.CloseFile(f);
    WriteLn('wrote ok');
  end;

  textfile.Assign(f, ParamStr(1));
  textfile.Reset(f);
  s := '';
  while not textfile.Eoln(f) do
  begin
    textfile.Read(f, c);
    s := s + c;
  end;
  WriteLn('chars=', s);
  textfile.Read(f, c);          { consume the line's newline }
  n := 0;
  while not textfile.Eof(f) do
  begin
    textfile.ReadLn(f, line);
    n := n + 1;
    WriteLn('L', n, '=', line);
  end;
  textfile.CloseFile(f);
  WriteLn('rest=', n);

  textfile.Assign(f, ParamStr(1) + '.nope');
  textfile.Reset(f);
  if not textfile.IOOk(f) then WriteLn('missing: open failed');
  if textfile.Eof(f) then WriteLn('missing: eof');
  textfile.CloseFile(f);
end.
