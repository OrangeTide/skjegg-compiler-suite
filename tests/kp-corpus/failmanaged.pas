program failmanaged;
{ `or else` recovering a managed result (string, dynamic array, map).  The value
  on success is the callee's owned result; the fallback on failure is made owned
  the way an assignment is: a reference type (string) is retained, a value type
  (dynamic array, map) is cloned so the recovered value stays independent of the
  fallback source.  Exercises writeln, assignment, a literal fallback, and a
  borrowed-variable fallback, and checks the clone is independent after the
  source is mutated.  Run under kprun it prints:
  pos none far pos far 3 30 0 77 77 42 55 55 }

procedure emit(v: integer); external;

function pick(n: integer): string fails;
begin
  if n < 0 then fail(1);
  if n = 0 then pick := 'none' else pick := 'pos';
end;

function nums(n: integer): array of integer fails;
begin
  if n < 0 then fail(2);
  SetLength(nums, 2);
  nums[0] := n;
  nums[1] := n * 10;
end;

function tally(n: integer): map of integer fails;
begin
  if n < 0 then fail(3);
  tally['k'] := n;
end;

var
  s, d: string;
  a, fa: array of integer;
  m, fm: map of integer;
begin
  d := 'far';

  { string: success returns the callee's owned value (the fallback is not
    evaluated); failure retains the borrowed-variable fallback }
  writeln(pick(1) or else 'lit');     { pos }
  writeln(pick(0) or else 'lit');     { none }
  writeln(pick(-1) or else d);        { far }

  s := pick(2) or else d;             { assign the owned success value }
  writeln(s);                         { pos }
  s := pick(-9) or else d;            { assign the retained fallback }
  writeln(s);                         { far }

  { dynamic array: the fallback is cloned so it is independent of its source }
  SetLength(fa, 1);
  fa[0] := 77;
  a := nums(3) or else fa;            { success: [3, 30] }
  emit(a[0]); emit(a[1]);             { 3 30 }
  a := nums(-1) or else fa;           { failure: a clone of fa }
  emit(High(a)); emit(a[0]);          { 0 77 }
  fa[0] := 88;                        { mutate the source: the clone is unchanged }
  emit(a[0]);                         { 77 }

  { map: same make-owned rule, the fallback cloned so it is independent }
  fm['k'] := 55;
  m := tally(42) or else fm;          { success: the callee's owned map }
  emit(m['k']);                       { 42 }
  m := tally(-1) or else fm;          { failure: a clone of fm }
  emit(m['k']);                       { 55 }
  fm['k'] := 66;                      { mutate the source: the clone is unchanged }
  emit(m['k']);                       { 55 }
end.
