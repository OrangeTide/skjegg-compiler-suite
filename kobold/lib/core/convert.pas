unit core.convert;
{ String-to-number conversion with a status, the failable face of the classic
  Val(s, v, code) builtin.  A console read is lenient (a non-numeric token reads
  as zero, with no way to tell a real zero from a parse that found nothing), so a
  program that wants to know whether the text was a number reads it as a string
  (io.console's ReadLine) and converts it here.  Typical use is a fallback with
  'or else', or a test with 'fails':

    n := ValInt(line) or else 0;
    if ValReal(line) fails then WriteLine('not a number');

  Both functions are failable: a clean parse returns the value, and a stray
  character or an empty string fails with the 1-based position of the offending
  character as the error code (the code Val reports).  Val is strict: it does not
  skip surrounding whitespace, so Trim the text first if the input may be padded.
  Built entirely over the Val and fail builtins, so the unit carries no runtime
  primitive of its own and works on every tier. }
interface

function ValInt(const s: string): Int64 fails;   { parse a signed decimal; fails on a bad token }
function ValReal(const s: string): real fails;    { parse a real; fails on a bad token }

implementation

function ValInt(const s: string): Int64 fails;
var
  v: Int64;
  code: Integer;
begin
  Val(s, v, code);
  if code <> 0 then
    fail(code);
  ValInt := v;
end;

function ValReal(const s: string): real fails;
var
  v: real;
  code: Integer;
begin
  Val(s, v, code);
  if code <> 0 then
    fail(code);
  ValReal := v;
end;

end.
