program varparam;
{ A var parameter passes the argument's address, so the callee writes through it
  and the caller sees the change. swap exchanges two integers in place; setpair
  fills two out parameters; accum adds into a var real. The by-reference path also
  reaches a record field and an array element, not only a plain variable.
  Run under kprun it prints 7 3 10 20 6.5 99 5. }
procedure emit(v: integer); external;
procedure emitf(v: real); external;

procedure swap(var a, b: integer);
var
  t: integer;
begin
  t := a; a := b; b := t;
end;

procedure setpair(out lo, hi: integer);
begin
  lo := 10; hi := 20;
end;

procedure accum(var acc: real; v: real);
begin
  acc := acc + v;
end;

type
  TPoint = record
    x, y: integer;
  end;

var
  p, q: integer;
  lo, hi: integer;
  total: real;
  pt: TPoint;
  arr: array[1..3] of integer;

begin
  p := 3; q := 7;
  swap(p, q);
  emit(p);                 { 7 }
  emit(q);                 { 3 }

  setpair(lo, hi);
  emit(lo);                { 10 }
  emit(hi);                { 20 }

  total := 2.0;
  accum(total, 1.5);
  accum(total, 3.0);
  emitf(total);            { 6.5 }

  pt.x := 1; pt.y := 99;
  swap(pt.x, pt.y);        { a record field by reference }
  emit(pt.x);              { 99, the fields exchanged }

  arr[1] := 8; arr[2] := 5;
  swap(arr[1], arr[2]);    { an array element by reference }
  emit(arr[1]);            { 5 }
end.
