{ A `cdecl` procedure type is a C-convention callback type for the FFI boundary. On
  every target the native and C conventions are ABI-identical, so a cdecl proc type is
  signature-compatible with a plain one and calls the same way. This exercises the
  keyword and that compatibility without a C side. }
program proccdecl;
type IntFn = function(x: integer): integer cdecl;
function dbl(x: integer): integer;
begin dbl := x * 2; end;
function apply(f: IntFn; n: integer): integer;
begin apply := f(n); end;
var c: IntFn;                              { cdecl }
    p: function(x: integer): integer;      { plain }
begin
  c := dbl;
  writeln(c(21));          { 42 }
  writeln(apply(dbl, 50)); { 100 }
  p := dbl;
  c := p;                  { plain -> cdecl }
  writeln(c(5));           { 10 }
  p := c;                  { cdecl -> plain }
  writeln(p(7));           { 14 }
end.
