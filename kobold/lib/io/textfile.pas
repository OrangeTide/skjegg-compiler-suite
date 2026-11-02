unit io.textfile;
{ Text file I/O in the classic ISO/Turbo Pascal idiom: Assign a name, Reset to
  read or Rewrite to write, then Read a character or ReadLn a line, WriteLn /
  Write output, test Eof / Eoln, and CloseFile when done.  The dialect has no
  built-in `text` type, so a file is the TextFile value below rather than a
  `var f: text`.

  This is a thin shim over io.files: the descriptors, host buffering, and the
  kpf_ runtime seam all live there, so this unit only maps the classic verbs onto
  that one implementation.  It reads through io.files.ReadChar, keeping a single
  character of look-ahead so Eof, Eoln, Read, and ReadLn stay in step the way the
  canonical model needs (Eof is true before the first Read on an empty file).
  Use io.files directly for the whole-file (ReadFile/WriteFile) and path-metadata
  (Exists/FileSize/Rename/...) surfaces; this unit is only the handle verbs.

  The verb names Read/Write/ReadLn/WriteLn also name console builtins, so a
  program reaches these through the unit qualifier (textfile.ReadLn(f, s)), not
  bare.  There is no IOResult in the dialect, so Reset and Rewrite record whether
  the open succeeded; IOOk reports it. }
uses io.files;
interface

type TextFile = record
  base: files.TextFile;   { the underlying io.files handle }
  name: string;           { the path bound by Assign }
  ok: boolean;            { the last Reset/Rewrite opened the file }
  havePeek: boolean;      { peekCh holds one character read ahead }
  peekCh: char;
  ended: boolean;         { a read-ahead has reached end of input }
end;

procedure Assign(var f: TextFile; const name: string);
procedure Reset(var f: TextFile);
procedure Rewrite(var f: TextFile);
function  IOOk(const f: TextFile): boolean;
function  Eof(var f: TextFile): boolean;
function  Eoln(var f: TextFile): boolean;
procedure Read(var f: TextFile; var ch: char);
procedure ReadLn(var f: TextFile; var line: string);
procedure Write(var f: TextFile; const s: string);
procedure WriteLn(var f: TextFile; const s: string);
procedure CloseFile(var f: TextFile);

implementation

{ Ensure one character of look-ahead is buffered.  Returns False once the stream
  is exhausted.  The look-ahead character is consumed by setting havePeek False;
  the next peek then pulls the following one. }
function peek(var f: TextFile): boolean;
begin
  if f.havePeek then
    peek := True
  else if f.ended then
    peek := False
  else
  begin
    f.havePeek := files.ReadChar(f.base, f.peekCh);
    if not f.havePeek then
      f.ended := True;
    peek := f.havePeek;
  end;
end;

{ Bind a name to the file.  No file is opened until Reset or Rewrite. }
procedure Assign(var f: TextFile; const name: string);
begin
  f.name := name;
  f.ok := False;
  f.havePeek := False;
  f.ended := True;
end;

{ Open the bound file for reading.  IOOk is true on success; a failed open reads
  as an immediate Eof. }
procedure Reset(var f: TextFile);
begin
  f.ok := files.Open(f.name, f.base);
  f.havePeek := False;
  f.ended := not f.ok;
end;

{ Create the bound file for writing, truncating an existing one.  IOOk reports
  whether the open succeeded. }
procedure Rewrite(var f: TextFile);
begin
  f.ok := files.Create(f.name, f.base);
  f.havePeek := False;
  f.ended := True;        { a write handle is never read }
end;

{ Whether the last Reset or Rewrite opened the file (the dialect has no
  IOResult). }
function IOOk(const f: TextFile): boolean;
begin
  IOOk := f.ok;
end;

{ True when no further character can be read.  Reports Eof before the first Read
  on an empty file, and `while not Eof(f) do ... Read(f, ch)` visits each
  character once. }
function Eof(var f: TextFile): boolean;
begin
  Eof := not peek(f);
end;

{ True at the end of a line: the next character is a line terminator (LF, or the
  CR of a CRLF pair), or the file has ended. }
function Eoln(var f: TextFile): boolean;
begin
  if not peek(f) then
    Eoln := True
  else
    Eoln := (f.peekCh = chr(10)) or (f.peekCh = chr(13));
end;

{ Read the next character into `ch`, raw: a newline or carriage return is
  returned as itself.  At end of input `ch` is the null character; guard with
  Eof. }
procedure Read(var f: TextFile; var ch: char);
begin
  if peek(f) then
  begin
    ch := f.peekCh;
    f.havePeek := False;
  end
  else
    ch := chr(0);
end;

{ Read the rest of the current line into `line`, without its terminator, and
  consume the newline.  One trailing carriage return is dropped, so a CRLF file
  reads the same as an LF file.  At end of input `line` is empty. }
procedure ReadLn(var f: TextFile; var line: string);
var more: boolean;
begin
  line := '';
  more := True;
  while more do
  begin
    if not peek(f) then
      more := False
    else if f.peekCh = chr(10) then
    begin
      f.havePeek := False;      { consume the newline }
      more := False;
    end
    else
    begin
      line := line + f.peekCh;
      f.havePeek := False;
    end;
  end;
  if (length(line) > 0) and (line[length(line)] = chr(13)) then
    line := copy(line, 1, length(line) - 1);
end;

{ Write a string with no added newline. }
procedure Write(var f: TextFile; const s: string);
begin
  files.WriteStr(f.base, s);
end;

{ Write a string followed by a newline. }
procedure WriteLn(var f: TextFile; const s: string);
begin
  files.WriteLine(f.base, s);
end;

{ Close the handle; safe to call more than once. }
procedure CloseFile(var f: TextFile);
begin
  files.Close(f.base);
  f.ok := False;
  f.havePeek := False;
  f.ended := True;
end;

end.
