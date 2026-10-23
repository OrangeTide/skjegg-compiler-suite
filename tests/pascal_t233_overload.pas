{ Overloading by arity and type (P5 stage B).  Routines sharing a name are
  disambiguated at the call site by the argument count, then by argument type;
  a forward declaration and its body are one routine, not an overload. }
program Overload;

procedure show(a, b: LongInt);
begin
  writeln('two ', a + b);
end;

procedure show(a, b, c: LongInt);
begin
  writeln('three ', a + b + c);
end;

function kind(n: LongInt): string;
begin
  kind := 'int';
end;

function kind(s: string): string;
begin
  kind := 'str:' + s;
end;

function dbl(x: LongInt): LongInt; forward;

procedure useDbl;
begin
  writeln(dbl(21));
end;

function dbl(x: LongInt): LongInt;
begin
  dbl := x * 2;
end;

begin
  show(1, 2);
  show(1, 2, 3);
  writeln(kind(5));
  writeln(kind('hi'));
  useDbl;
end.
