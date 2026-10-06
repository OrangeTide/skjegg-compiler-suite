program fib;
var
    i: integer;

function fib(n: integer): integer;
begin
    if n < 2 then
        fib := n
    else
        fib := fib(n - 1) + fib(n - 2);
end;

begin
    for i := 0 to 10 do
        WriteLn('fib(', i, ') = ', fib(i));
end.
