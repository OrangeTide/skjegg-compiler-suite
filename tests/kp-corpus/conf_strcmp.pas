program conf_strcmp;
{ Conformance: strings compare by unsigned bytewise lexicographic order, with a
  shorter string that is a prefix of a longer one sorting first, and equality by
  length then bytes (kp-evaluation.md, "Strings"; kp-semantics.md). The #200 byte
  pins the UNSIGNED comparison: a signed byte would make it negative and order it
  before 'a'. Run under kprun it prints 1 0 1 0 1 1 1 0 1 0. }
procedure emit(v: integer); external;

var
  s: string;
begin
  emit(Ord('abc' < 'abd'));    { 1 }
  emit(Ord('abd' < 'abc'));    { 0 }
  emit(Ord('ab' < 'abc'));     { 1, shorter prefix sorts first }
  emit(Ord('abc' < 'ab'));     { 0 }
  emit(Ord('abc' = 'abc'));    { 1 }
  emit(Ord('abc' <> 'abd'));   { 1 }
  emit(Ord('a' < #200));       { 1, unsigned: 97 < 200 }
  emit(Ord(#200 < 'a'));       { 0 }
  s := 'm';
  emit(Ord(s < 'n'));          { 1 }
  emit(Ord('n' < s));          { 0 }
end.
