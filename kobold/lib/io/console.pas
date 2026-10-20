unit io.console;
{ The console as a unit a program `uses`, rather than compiler builtins or
  external declarations repeated in every program.  It presents the standard
  input and output as ordinary typed functions over the console runtime seam
  (kobold/io_free.c for the freestanding tier, kobold/kp_host.c for the hosted
  JIT and AOT, same symbols the write/read statement builtins lower to).

  Eof is the sticky end-of-input latch: it reads one byte ahead over the read
  seam and stays true once the stream has ended, so a read-to-end loop stops
  once instead of spinning on a zero-length read.  ReadLine reads the current
  line (consuming its newline), so a `while not Eof do ReadLine` loop walks the
  input a line at a time, delivering a newline-less final line before Eof
  latches.  The Write* procedures go to standard output.

  Numeric and real console I/O (ReadInt, ReadReal, WriteReal) are a later
  addition: a real-returning external cdecl is not yet marshalled on the rv
  soft-float tier (the brv_* bits-bridge, see std.random's RandomReal), and the
  numeric reads carry Pascal's whitespace-skipping Val semantics that want their
  own design pass. }
interface

function Eof: Boolean;                 { true once no byte remains on standard input }
function ReadLine: string;             { the current line, its newline consumed }
function ReadChar: char;               { the next character; guard with Eof at end of input }

procedure WriteStr(const s: string);   { write s to standard output, no newline }
procedure WriteLine(const s: string);  { write s and a newline to standard output }
procedure WriteChar(c: char);          { write one character to standard output }

implementation

{ The console primitives, declared once here instead of in every program.  fd 1
  is standard output; the reads act on standard input. }
function io_eof: Boolean; external cdecl;
function io_read_str: string; external cdecl;
function io_read_char: integer; external cdecl;
procedure io_write_str(fd: integer; const s: string); external cdecl;
procedure io_write_char(fd: integer; ch: integer); external cdecl;

function Eof: Boolean;
begin
  Eof := io_eof;
end;

function ReadLine: string;
begin
  ReadLine := io_read_str;
end;

function ReadChar: char;
begin
  ReadChar := Chr(io_read_char);
end;

procedure WriteStr(const s: string);
begin
  io_write_str(1, s);
end;

procedure WriteLine(const s: string);
begin
  io_write_str(1, s);
  io_write_char(1, 10);                { newline }
end;

procedure WriteChar(c: char);
begin
  io_write_char(1, Ord(c));
end;

end.
