unit io.buffered;
{ Buffered I/O as a unit a program `uses`, built entirely in Kobold over the raw
  fd seam.  The runtime's console primitives are unbuffered and keep only the one
  byte of input pushback the numeric and line reads need (see kp-embedding.md);
  any larger buffering, output batching, or a general ungetc is an application or
  library concern, and this unit is that library.  It stands on the fd seam alone:
  io_write_str (a plain write of some bytes to a descriptor) and kpf_read (a
  stateless chunk read from one), the same primitives io.console and io.files use.

  A stream owns the descriptor it is given.  Do not mix a buffered stream with the
  console builtins or with a second stream on the same fd: each keeps its own view
  of the stream position, as two stdio FILE* over one fd would.

  The writer batches PutChar/PutStr into one write per bufferful (or per Flush),
  so a run of small writes costs few syscalls instead of one each; call Flush
  before the descriptor is closed.  The reader pulls host chunks and serves bytes
  and lines from them, with an arbitrary-length UngetStr pushback (any bytes, any
  number of times), the general lookahead the single-byte runtime pushback does
  not promise. }
interface

type
  BufWriter = record
    fd: integer;      { the descriptor written to; owned by the caller }
    data: string;     { bytes accumulated here, not yet written }
  end;
  BufReader = record
    fd: integer;      { the descriptor read from; owned by the caller }
    data: string;     { bytes pulled from the fd, not yet consumed }
    done: boolean;    { a host read has reached end of input }
  end;

procedure OpenWriter(var w: BufWriter; fd: integer);
procedure PutStr(var w: BufWriter; const s: string);
procedure PutChar(var w: BufWriter; c: char);
procedure Flush(var w: BufWriter);

procedure OpenReader(var r: BufReader; fd: integer);
function GetChar(var r: BufReader; var c: char): boolean;   { False at end of input }
procedure UngetStr(var r: BufReader; const s: string);      { push bytes back for the next read }
function ReadLine(var r: BufReader; var line: string): boolean;   { False at end of input }
function Eof(var r: BufReader): boolean;

implementation

const BUF_CAP = 4096;   { flush the writer once it holds this many bytes }

{ The raw fd seam, declared here so this unit stands on it alone. }
procedure io_write_str(fd: integer; const s: string); external cdecl;
function kpf_read(fd: integer): string; external cdecl;

procedure OpenWriter(var w: BufWriter; fd: integer);
begin
  w.fd := fd;
  w.data := '';
end;

{ Write the accumulated bytes out in one call and clear the buffer. }
procedure Flush(var w: BufWriter);
begin
  if length(w.data) > 0 then
  begin
    io_write_str(w.fd, w.data);
    w.data := '';
  end;
end;

procedure PutStr(var w: BufWriter; const s: string);
begin
  w.data := w.data + s;
  if length(w.data) >= BUF_CAP then
    Flush(w);
end;

procedure PutChar(var w: BufWriter; c: char);
begin
  PutStr(w, '' + c);
end;

procedure OpenReader(var r: BufReader; fd: integer);
begin
  r.fd := fd;
  r.data := '';
  r.done := False;
end;

{ Append one host chunk to the buffer, setting done at end of input. }
procedure Fill(var r: BufReader);
var chunk: string;
begin
  if not r.done then
  begin
    chunk := kpf_read(r.fd);
    if length(chunk) = 0 then
      r.done := True
    else
      r.data := r.data + chunk;
  end;
end;

function GetChar(var r: BufReader; var c: char): boolean;
begin
  if (length(r.data) = 0) and (not r.done) then
    Fill(r);
  if length(r.data) > 0 then
  begin
    c := r.data[1];
    r.data := copy(r.data, 2, length(r.data) - 1);
    GetChar := True;
  end
  else
  begin
    c := chr(0);
    GetChar := False;
  end;
end;

{ Push bytes back so the next read returns them first.  Any length, any number of
  times: the general pushback the runtime's single byte does not give. }
procedure UngetStr(var r: BufReader; const s: string);
begin
  r.data := s + r.data;
end;

{ Strip one trailing carriage return, so a CRLF line reads the same as an LF one
  (matching io.files). }
function stripcr(const s: string): string;
begin
  if (length(s) > 0) and (s[length(s)] = chr(13)) then
    stripcr := copy(s, 1, length(s) - 1)
  else
    stripcr := s;
end;

{ Read the next line (without its newline) into `line`.  False at end of input; a
  final line with no trailing newline is still returned. }
function ReadLine(var r: BufReader; var line: string): boolean;
var nl: string;
    at: integer;
begin
  nl := '' + chr(10);
  at := pos(nl, r.data);
  while (at = 0) and (not r.done) do
  begin
    Fill(r);
    at := pos(nl, r.data);
  end;
  if at > 0 then
  begin
    line := stripcr(copy(r.data, 1, at - 1));
    r.data := copy(r.data, at + 1, length(r.data) - at);
    ReadLine := True;
  end
  else if length(r.data) > 0 then
  begin
    line := stripcr(r.data);
    r.data := '';
    ReadLine := True;
  end
  else
  begin
    line := '';
    ReadLine := False;
  end;
end;

{ True once no byte remains and the input has ended. }
function Eof(var r: BufReader): boolean;
begin
  if (length(r.data) = 0) and (not r.done) then
    Fill(r);
  Eof := r.done and (length(r.data) = 0);
end;

end.
