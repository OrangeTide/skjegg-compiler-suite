program convertnum;
{ Exercise core.convert: the failable string-to-number surface.  A clean parse
  returns the value; a bad token fails and is caught with `fails` and `or else`,
  the error code carrying the 1-based position Val reports.  Val is strict (no
  whitespace skipping, FPC semantics), so '12x' fails at 3 and '' fails at 1. }
uses io.console as _, core.convert as _;

procedure ShowInt(const s: string);
var
  n: Int64;
  ns: string;
begin
  if ValInt(s) fails e then begin
    Str(errorcode(e), ns);
    WriteLine('int "' + s + '" bad at ' + ns);
  end
  else begin
    n := ValInt(s) or else 0;
    Str(n, ns);
    WriteLine('int "' + s + '" = ' + ns);
  end;
end;

procedure ShowReal(const s: string);
var
  x: real;
begin
  if ValReal(s) fails then
    WriteLine('real "' + s + '" bad')
  else begin
    x := ValReal(s) or else 0.0;
    WriteStr('real "' + s + '" = ');
    WriteReal(x);
    WriteLine('');
  end;
end;

var
  fv: Int64;
  fs: string;
begin
  ShowInt('42');
  ShowInt('-7');
  ShowInt('12x');
  ShowInt('');
  ShowReal('3.5');
  ShowReal('1.5e2');
  ShowReal('nope');
  fv := ValInt('bad') or else -1;
  Str(fv, fs);
  WriteLine('fallback ' + fs);
end.
