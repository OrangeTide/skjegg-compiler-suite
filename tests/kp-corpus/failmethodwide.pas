program failmethodwide;
{ Failable methods returning a real, an int64, and a record: the value travels through the
  hidden caller buffer (sret) and the error in the return register, the same wide path a
  failable free function uses, now reached through a record method whose receiver is passed by
  reference. Exercises or else on both paths, the fails test with errorcode, and or fail
  propagation through a wrapper method. Run under kprun it prints
  2.5 -1 3 -9 1 3000000000 7 4000000000 2 5 10 100 200 6 12 3, one per line. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;
procedure emitf(v: real); external;

type Box = record n: integer; end;
type Point = record x, y: integer; end;

function half for (b: Box): real fails;
begin
  if b.n < 0 then fail(1);
  half := b.n / 2;
end;

function half2 for (b: Box): real fails;
begin
  half2 := b.half or fail;
end;

function big for (b: Box): int64 fails;
begin
  if b.n < 0 then fail(2);
  big := b.n;
  big := big * 1000000000;
end;

function big2 for (b: Box): int64 fails;
begin
  big2 := b.big or fail;
end;

function mk for (b: Box): Point fails;
var t: Point;
begin
  if b.n < 0 then fail(3);
  t.x := b.n; t.y := b.n * 2;
  mk := t;
end;

function mk2 for (b: Box): Point fails;
begin
  mk2 := b.mk or fail;
end;

var
  b: Box;
  p, def: Point;
begin
  b.n := 5;  emitf(b.half or else -1.0);    { 2.5 }
  b.n := -5; emitf(b.half or else -1.0);     { -1 }
  b.n := 6;  emitf(b.half2 or else -1.0);    { 3 }
  b.n := -6; emitf(b.half2 or else -9.0);    { -9 }
  b.n := -1;
  if b.half fails e1 then emit(errorcode(e1)) else emit(0);   { 1 }

  b.n := 3;  emit64(b.big or else 0);        { 3000000000 }
  b.n := -3; emit64(b.big or else 7);        { 7 }
  b.n := 4;  emit64(b.big2 or else 0);       { 4000000000 }
  b.n := -9;
  if b.big fails e2 then emit(errorcode(e2)) else emit(0);    { 2 }

  def.x := 100; def.y := 200;
  b.n := 5;  p := b.mk or else def;  emit(p.x); emit(p.y);    { 5 10 }
  b.n := -5; p := b.mk or else def;  emit(p.x); emit(p.y);    { 100 200 }
  b.n := 6;  p := b.mk2 or else def; emit(p.x); emit(p.y);    { 6 12 }
  b.n := -1;
  if b.mk fails e3 then emit(errorcode(e3)) else emit(0);     { 3 }
end.
