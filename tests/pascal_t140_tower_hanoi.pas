{ Test Tower of Hanoi }
program TestHanoi;
var
  moves: integer;

procedure hanoi(n: integer; src, dst, aux: char);
begin
  if n = 1 then begin
    write(src);
    write(' -> ');
    writeln(dst);
    moves := moves + 1;
  end else begin
    hanoi(n - 1, src, aux, dst);
    write(src);
    write(' -> ');
    writeln(dst);
    moves := moves + 1;
    hanoi(n - 1, aux, dst, src);
  end;
end;

begin
  moves := 0;
  hanoi(3, 'A', 'C', 'B');
  writeln(moves);
end.
