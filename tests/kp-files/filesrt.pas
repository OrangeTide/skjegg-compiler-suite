program filesrt;
{ File I/O reached through the `files` unit: no per-program external
  declarations, just `uses io.files`.  Exercises the surfaces on paths derived
  from the first argument: the whole-file ReadFile/WriteFile, the Pascal-style
  line/stream handle (Create/WriteLine, then Open/ReadLine), and the path
  metadata and operations (Exists/FileSize/IsDir and Delete/Rename/MakeDir/
  RemoveDir).  The metadata part is self-cleaning so the three tiers agree. }
uses io.files;
var
  f: files.TextFile;
  line: string;
  n: integer;
  sz: Int64;

function yn(b: boolean): string;
begin
  if b then yn := 'yes' else yn := 'no';
end;

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

  { Path metadata and operations.  rt.txt now holds "one\ntwo\nthree\n" = 14 bytes. }
  WriteLn('exists=', yn(files.Exists(ParamStr(1))));
  sz := files.FileSize(ParamStr(1));
  WriteLn('size=', sz);
  WriteLn('isdir=', yn(files.IsDir(ParamStr(1))));
  WriteLn('nope=', yn(files.Exists(ParamStr(1) + '.nope')));

  if files.RenameFile(ParamStr(1), ParamStr(1) + '.bak') then
    WriteLn('renamed=', yn(files.Exists(ParamStr(1) + '.bak')),
            ' gone=', yn(not files.Exists(ParamStr(1))));
  files.RenameFile(ParamStr(1) + '.bak', ParamStr(1));   { restore the name }

  if files.MakeDir(ParamStr(1) + '.d') then
    WriteLn('mkdir isdir=', yn(files.IsDir(ParamStr(1) + '.d')));
  WriteLn('rmdir=', yn(files.RemoveDir(ParamStr(1) + '.d')));

  WriteLn('delete=', yn(files.DeleteFile(ParamStr(1))),
          ' gone=', yn(not files.Exists(ParamStr(1))));
end.
