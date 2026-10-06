program incdecl;
{ An include may splice a whole declaration, not just an expression: incbody.inc
  supplies the Accumulate procedure spliced between the var block and the body. }
var
  total: integer;
{$I incbody.inc}
begin
  total := 0;
  Accumulate(total, 10);
  Accumulate(total, 20);
  WriteLn('total=', total);
end.
