{ A standalone self-checking program for the skjcc cross driver's Pascal
  path: a computation whose result is the process exit code.  Halts with 42. }
program driver_hello;
var
    s, i: integer;
begin
    s := 0;
    for i := 0 to 8 do
        s := s + i;             { 0+1+...+8 = 36 }
    if s = 36 then
        halt(42)
    else
        halt(1);
end.
