program ShortCircuitAnd;
var a, b: boolean;
begin
  a := false;
  b := true;
  { 'and then' should skip second operand when first is false }
  if a and then b then
    writeln('FAIL')
  else
    writeln('OK1');
  a := true;
  b := true;
  if a and then b then
    writeln('OK2')
  else
    writeln('FAIL');
  a := true;
  b := false;
  if a and then b then
    writeln('FAIL')
  else
    writeln('OK3')
end.
