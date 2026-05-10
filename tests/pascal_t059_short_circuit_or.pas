program ShortCircuitOr;
var a, b: boolean;
begin
  a := true;
  b := false;
  { 'or else' should skip second operand when first is true }
  if a or else b then
    writeln('OK1')
  else
    writeln('FAIL');
  a := false;
  b := false;
  if a or else b then
    writeln('FAIL')
  else
    writeln('OK2');
  a := false;
  b := true;
  if a or else b then
    writeln('OK3')
  else
    writeln('FAIL')
end.
