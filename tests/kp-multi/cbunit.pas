unit cbunit;
{ Cross-unit export of a procedure (callback) type: the exported Op type and routines
  that take an Op, so a consumer passes one of its own routines (a code pointer) across
  the unit boundary. }
interface
type
  Op = function(x: integer): integer;
function Apply(f: Op; n: integer): integer;
function Twice(f: Op; n: integer): integer;
implementation
function Apply(f: Op; n: integer): integer;
begin Apply := f(n); end;
function Twice(f: Op; n: integer): integer;
begin Twice := f(f(n)); end;
end.
