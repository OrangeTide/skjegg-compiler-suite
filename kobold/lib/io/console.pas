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

  ReadReal and WriteReal are the real console surface: ReadReal skips leading
  whitespace and reads one real token (Pascal's read(x) for a real), WriteReal
  prints it in the "%g" shape the write statement uses.  A real crosses the
  cdecl FFI the same way the write/read statement's io_*_real builtins do (in a
  float register on ColdFire, as its 64-bit bits in an integer register pair on
  the rv32 soft-float tier), so these need no bridge.

  ReadInt and WriteInt are the integer console surface: ReadInt skips leading
  whitespace and reads one signed decimal token (Pascal's read(i)), returning an
  Int64 so a whole number up to 64 bits parses without truncation; WriteInt
  prints a 32-bit Integer.  ReadInt rides the same 64-bit cdecl return as
  std.time's Now, so it is endian-safe on both tiers.  A non-numeric input reads
  as zero (this tier's lenient scan, the same the read statement uses); the
  status-returning Val surface is a later design pass. }
interface

function Eof: Boolean;                 { true once no byte remains on standard input }
function ReadLine: string;             { the current line, its newline consumed }
function ReadChar: char;               { the next character; guard with Eof at end of input }
function ReadReal: real;               { skip whitespace and read one real token }
function ReadInt: Int64;               { skip whitespace and read one signed decimal token }

procedure WriteStr(const s: string);   { write s to standard output, no newline }
procedure WriteLine(const s: string);  { write s and a newline to standard output }
procedure WriteChar(c: char);          { write one character to standard output }
procedure WriteReal(x: real);          { write x to standard output in "%g" shape, no newline }
procedure WriteInt(x: Integer);        { write x to standard output in decimal, no newline }

implementation

{ The console primitives, declared once here instead of in every program.  fd 1
  is standard output; the reads act on standard input. }
function io_eof: Boolean; external cdecl;
function io_read_str: string; external cdecl;
function io_read_char: integer; external cdecl;
procedure io_write_str(fd: integer; const s: string); external cdecl;
procedure io_write_char(fd: integer; ch: integer); external cdecl;
function io_read_real: real; external cdecl;
procedure io_write_real(fd: integer; x: real); external cdecl;
function io_read_int: Int64; external cdecl;
procedure io_write_int(fd: integer; v: integer); external cdecl;

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

function ReadReal: real;
begin
  ReadReal := io_read_real;
end;

procedure WriteReal(x: real);
begin
  io_write_real(1, x);
end;

function ReadInt: Int64;
begin
  ReadInt := io_read_int;
end;

procedure WriteInt(x: Integer);
begin
  io_write_int(1, x);
end;

end.
