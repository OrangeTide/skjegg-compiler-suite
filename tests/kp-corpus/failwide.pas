program failwide;
{ Failable functions returning a real, an int64, and a record (the value travels through a
  hidden caller buffer, the error in the return register). Exercises or else on both paths,
  the fails test with errorcode, and or fail propagation through a wrapper. A negative 32-bit
  `or else` fallback into an Int64 result must sign-extend, not zero-extend (else -1 reads as
  4294967295 on a 64-bit backend). Run under kprun it prints
  2.5 -1 3 -9 1 3000000000 7 4000000000 2 -1 5 10 100 200 6 12 3, one per line. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;
procedure emitf(v: real); external;

function half(n: integer): real fails;
begin
  if n < 0 then fail(1);
  half := n / 2;
end;

function half2(n: integer): real fails;
begin
  half2 := half(n) or fail;
end;

function big(n: integer): int64 fails;
begin
  if n < 0 then fail(2);
  big := n;
  big := big * 1000000000;
end;

function big2(n: integer): int64 fails;
begin
  big2 := big(n) or fail;
end;

type Point = record x, y: integer; end;

function mk(n: integer): Point fails;
var t: Point;
begin
  if n < 0 then fail(3);
  t.x := n; t.y := n * 2;
  mk := t;
end;

function mk2(n: integer): Point fails;
begin
  mk2 := mk(n) or fail;
end;

var
  p, def: Point;
begin
  emitf(half(5) or else -1.0);
  emitf(half(-5) or else -1.0);
  emitf(half2(6) or else -1.0);
  emitf(half2(-6) or else -9.0);
  if half(-1) fails e1 then emit(errorcode(e1)) else emit(0);

  emit64(big(3) or else 0);
  emit64(big(-3) or else 7);
  emit64(big2(4) or else 0);
  if big(-9) fails e2 then emit(errorcode(e2)) else emit(0);
  emit64(big(-3) or else -1);

  def.x := 100; def.y := 200;
  p := mk(5) or else def;
  emit(p.x); emit(p.y);
  p := mk(-5) or else def;
  emit(p.x); emit(p.y);
  p := mk2(6) or else def;
  emit(p.x); emit(p.y);
  if mk(-1) fails e3 then emit(errorcode(e3)) else emit(0);
end.
