program mu_overload_value;
{ Take the value of an overloaded routine, through a qualified unit reference
  (@ov.Tag) and a bare one (@Loc). A routine value carries no call arguments, so
  the overload is chosen by the target proc type: the variable it is assigned to
  or the parameter it is passed to. A wrong choice would assign or pass the wrong
  overload and either fail to type-check or call the wrong body.

  Both the assignment context (gi := @ov.Tag) and the call-argument context
  (apply_i(@ov.Tag, ...)) are exercised, for the qualified and the bare form. }
uses ovldep as ov;

type
  Fi = function(n: integer): string;
  Fs = function(s: string): string;
  Fb = function(b: boolean): string;

{ Bare overloads, resolved the same way as the unit's. }
function Loc(n: integer): string;
begin
  if n < 0 then exit('loc:neg');
  exit('loc:nonneg');
end;

function Loc(s: string): string;
begin
  exit('loc:' + s);
end;

function apply_i(f: Fi; n: integer): string;
begin
  exit(f(n));
end;

function apply_s(f: Fs; const s: string): string;
begin
  exit(f(s));
end;

var
  gi: Fi;
  gs: Fs;
  gb: Fb;
  li: Fi;
  ls: Fs;
begin
  { qualified value, assignment context: one name, three target types }
  gi := @ov.Tag;
  gs := @ov.Tag;
  gb := @ov.Tag;
  writeln(gi(7));
  writeln(gi(-1));
  writeln(gs('hi'));
  writeln(gb(true));
  { qualified value, call-argument context }
  writeln(apply_i(@ov.Tag, 42));
  writeln(apply_s(@ov.Tag, 'arg'));
  { bare value, assignment and argument contexts }
  li := @Loc;
  ls := @Loc;
  writeln(li(-5));
  writeln(ls('bare'));
  writeln(apply_i(@Loc, 3));
end.
