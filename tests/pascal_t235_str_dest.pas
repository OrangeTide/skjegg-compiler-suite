{ Str(v, dest) with the destination being the enclosing function's string
  result (function-name-as-var), as well as a plain string variable and a
  record field source (P5 stage D).  Also writing a string-valued expression
  (a concatenation) directly. }
program StrDest;
type
  TRec = record
    line: LongInt;
  end;

function intToStr(v: LongInt): string;
begin
  Str(v, intToStr);          { destination is the function result }
end;

var
  r: TRec;
  s: string;
begin
  r.line := 123;
  Str(r.line, s);            { destination is a plain string variable }
  writeln(s);
  writeln(intToStr(-7));
  writeln('n=' + intToStr(42));
end.
