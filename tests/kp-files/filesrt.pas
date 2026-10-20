program filesrt;
{ File I/O reached through the `files` unit: no per-program external
  declarations, just `uses files`.  Exercises both surfaces on the file named by
  the first argument: the whole-file ReadFile/WriteFile, and the Pascal-style
  line/stream handle (Create/WriteLine, then Open/ReadLine). }
uses files;
var
  f: files.TextFile;
  line: string;
  n: integer;
begin
  if files.WriteFile(ParamStr(1), 'hello from a unit') then
    WriteLn('write ok');
  WriteLn(files.ReadFile(ParamStr(1)));

  if files.Create(ParamStr(1), f) then
  begin
    files.WriteLine(f, 'one');
    files.WriteLine(f, 'two');
    files.WriteLine(f, 'three');
    files.Close(f);
  end;
  n := 0;
  if files.Open(ParamStr(1), f) then
  begin
    while files.ReadLine(f, line) do
    begin
      n := n + 1;
      WriteLn('L', n, '=', line);
    end;
    files.Close(f);
  end;
  WriteLn('lines=', n);
end.
