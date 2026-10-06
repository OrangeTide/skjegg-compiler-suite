program singletype;
{ The single type is a 4-byte f32 storage format. A value is narrowed to single
  precision on store (FSS) and widened to the f64 held in registers on load (FLS);
  computation is in f64. 16777217 (2^24 + 1) is not representable in f32 and
  rounds to 16777216, so the difference after a round trip through a single is 0.
  A single record field is 4 bytes, leaving the next field intact. Run under
  kprun it prints 0 42 3.5 4 0. }
procedure emitf(v: real); external;
procedure emit(v: integer); external;

type
  TRec = record
    a: single;
    n: integer;
  end;

var
  s: single;
  d: real;
  r: TRec;
  arr: array[1..3] of single;

begin
  s := 16777217.0;
  d := s - 16777216.0;
  emitf(d);                { 0, the value narrowed to single on store }

  r.a := 3.5; r.n := 42;
  emit(r.n);               { 42, the single field did not overrun its neighbour }
  emitf(r.a);              { 3.5 }

  arr[1] := 1.5; arr[2] := 2.5; arr[3] := 16777217.0;
  emitf(arr[1] + arr[2]);  { 4 }
  d := arr[3] - 16777216.0;
  emitf(d);                { 0 }
end.
