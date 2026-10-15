program conf_setrel;
{ Conformance: small-set relations and operators (kp-evaluation.md, "Sets";
  kp-semantics.md). <= is subset, < proper subset, >= superset, > proper
  superset, = and <> equality, in membership; + - * are union, difference, and
  intersection. Run under kprun it prints 1 1 0 1 1 1 0 1 1 1 0 1 1 0 1. }
procedure emit(v: integer); external;

type
  Small = 0..7;

var
  s, t, u: set of Small;
begin
  s := [1, 3, 5];
  t := [1, 3, 5, 7];
  emit(Ord(s <= t));    { 1 subset }
  emit(Ord(s < t));     { 1 proper subset }
  emit(Ord(t <= s));    { 0 }
  emit(Ord(t >= s));    { 1 superset }
  emit(Ord(t > s));     { 1 proper superset }
  emit(Ord(s >= s));    { 1 }
  emit(Ord(s > s));     { 0 not proper }
  emit(Ord(s = s));     { 1 }
  emit(Ord(s <> t));    { 1 }
  emit(Ord(3 in s));    { 1 }
  emit(Ord(2 in s));    { 0 }

  u := s + [2];         { union }
  emit(Ord(2 in u));    { 1 }
  u := t - s;           { difference, leaves just 7 }
  emit(Ord(7 in u));    { 1 }
  emit(Ord(1 in u));    { 0 }
  u := s * t;           { intersection, leaves 1, 3, 5 }
  emit(Ord(s = u));     { 1 }
end.
