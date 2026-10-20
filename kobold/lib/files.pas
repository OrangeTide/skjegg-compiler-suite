unit files;
{ File I/O as a unit a program `uses`, rather than a compiler builtin or an
  external declaration repeated in every program.  The compiler core carries no
  file knowledge: the primitives are ordinary external symbols the runtime
  provides (ReadFileStr / WriteFileStr and the kpf_* line/stream primitives, in
  kobold/aot_rt.c, kobold/io_free.c and the JIT binding table), declared once
  here and wrapped in a typed Kobold interface.

  The whole-file surface is ReadFile / WriteFile.  The handle surface is the
  Pascal-style one the classic `file`/`text` type would give, without a file
  type (the dialect omits those by design, kp-conformance.md): Open / Create a
  TextFile, then ReadLine until it returns False, or WriteStr / WriteLine, then
  Close.  Line buffering is done here in Kobold over a stateless chunk-read
  primitive, so the runtime seam stays minimal and the same on every tier. }
interface

type TextFile = record
  fd: integer;       { the open descriptor, -1 when closed }
  pending: string;   { bytes read from the host but not yet returned as lines }
  done: boolean;     { the host read reached end of file }
end;

function ReadFile(const path: string): string;
function WriteFile(const path, data: string): boolean;

function Open(const path: string; var f: TextFile): boolean;
function Create(const path: string; var f: TextFile): boolean;
function ReadLine(var f: TextFile; var line: string): boolean;
function AtEof(const f: TextFile): boolean;
procedure WriteStr(var f: TextFile; const s: string);
procedure WriteLine(var f: TextFile; const s: string);
procedure Close(var f: TextFile);

implementation

{ The host primitives, declared once here instead of in every program. }
function ReadFileStr(const path: string): string; external cdecl;
function WriteFileStr(const path, data: string): LongInt; external cdecl;
function kpf_open_read(const path: string): integer; external cdecl;
function kpf_open_write(const path: string): integer; external cdecl;
function kpf_read(fd: integer): string; external cdecl;
function kpf_write(fd: integer; const data: string): integer; external cdecl;
procedure kpf_close(fd: integer); external cdecl;

{ Read a whole file and return its contents, or the empty string if it cannot
  be read. }
function ReadFile(const path: string): string;
begin
  ReadFile := ReadFileStr(path);
end;

{ Write data to a file, truncating or creating it.  Returns True on success. }
function WriteFile(const path, data: string): boolean;
begin
  WriteFile := WriteFileStr(path, data) <> 0;
end;

{ Open a file for reading (the `reset` analog).  True on success. }
function Open(const path: string; var f: TextFile): boolean;
begin
  f.fd := kpf_open_read(path);
  f.pending := '';
  f.done := False;
  Open := f.fd >= 0;
end;

{ Create a file for writing, truncating or creating it (the `rewrite` analog). }
function Create(const path: string; var f: TextFile): boolean;
begin
  f.fd := kpf_open_write(path);
  f.pending := '';
  f.done := True;        { a write handle is never read from }
  Create := f.fd >= 0;
end;

{ Pull one host chunk into the pending buffer.  Sets done at end of file. }
procedure Fill(var f: TextFile);
var chunk: string;
begin
  if not f.done then
  begin
    chunk := kpf_read(f.fd);
    if length(chunk) = 0 then
      f.done := True
    else
      f.pending := f.pending + chunk;
  end;
end;

{ Strip one trailing carriage return, so a CRLF file reads the same as LF. }
function stripcr(const s: string): string;
begin
  if (length(s) > 0) and (s[length(s)] = chr(13)) then
    stripcr := copy(s, 1, length(s) - 1)
  else
    stripcr := s;
end;

{ Read the next line into `line` (without its newline).  Returns False at end of
  file.  A final line with no trailing newline is returned. }
function ReadLine(var f: TextFile; var line: string): boolean;
var nl: string;
    at: integer;
begin
  nl := '' + chr(10);
  at := pos(nl, f.pending);
  while (at = 0) and (not f.done) do
  begin
    Fill(f);
    at := pos(nl, f.pending);
  end;
  if at > 0 then
  begin
    line := stripcr(copy(f.pending, 1, at - 1));
    f.pending := copy(f.pending, at + 1, length(f.pending) - at);
    ReadLine := True;
  end
  else if length(f.pending) > 0 then
  begin
    line := stripcr(f.pending);
    f.pending := '';
    ReadLine := True;
  end
  else
  begin
    line := '';
    ReadLine := False;
  end;
end;

{ True once every line has been read. }
function AtEof(const f: TextFile): boolean;
begin
  AtEof := f.done and (length(f.pending) = 0);
end;

{ Write a string with no added newline. }
procedure WriteStr(var f: TextFile; const s: string);
begin
  kpf_write(f.fd, s);
end;

{ Write a string followed by a newline. }
procedure WriteLine(var f: TextFile; const s: string);
begin
  kpf_write(f.fd, s + chr(10));
end;

{ Close the handle; safe to call more than once. }
procedure Close(var f: TextFile);
begin
  if f.fd >= 0 then
    kpf_close(f.fd);
  f.fd := -1;
  f.pending := '';
  f.done := True;
end;

end.
