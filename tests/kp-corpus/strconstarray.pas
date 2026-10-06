program strconstarray;
{ Constant arrays of strings, the runtime-initialized aggregate. Unlike an ordinal constant
  array (a static image), a string constant array holds handles, so it is a zeroed global slot
  array filled when main starts: each element string is created and stored once, owned by the
  array for the program's lifetime. It is indexed by an enum or an integer, an element is a
  string used normally (Length, character index, whole-value read), the way kpcom's TokenName
  and NodeKindName tables work. Run under kprun it prints 3 5 4 103 3 4 1 98. }
procedure emit(v: integer); external;

type
  Color = (Red, Green, Blue);

const
  names: array[Color] of string = ('red', 'green', 'blue');
  words: array[1..3] of string = ('foo', 'quux', 'z');

function firstchar(c: Color): integer;
begin
  firstchar := Ord(names[c][1]);
end;

var
  c: Color;
  i: integer;
  s: string;
begin
  for c := Red to Blue do emit(Length(names[c]));   { 3 5 4 }
  emit(firstchar(Green));                            { 103, 'g' }
  for i := 1 to 3 do emit(Length(words[i]));         { 3 4 1 }
  s := names[Blue];
  emit(Ord(s[1]));                                   { 98, 'b' }
end.
