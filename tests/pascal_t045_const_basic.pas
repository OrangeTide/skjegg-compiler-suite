program t045_const_basic;

const
  MaxSize = 100;
  MinSize = 10;
  Range = MaxSize - MinSize;
  Doubled = MaxSize * 2;
  Half = MaxSize div 2;
  Remainder = MaxSize mod 7;
  NegVal = -42;
  Flags = $FF and $0F;
  Bits = $01 or $F0;
  Xored = ($FF or $0F) and (not ($FF and $0F));
  IsEqual = ord(MaxSize = 100);
  IsLess = ord(MinSize < MaxSize);
  NotTrue = ord(not true);
  Letter = ord('A');
  Computed = (MaxSize + MinSize) * 2;
  Sqr9 = sqr(9);
  Abs42 = abs(-42);
  IsOdd = ord(odd(7));
  IsEven = ord(odd(8));
  SuccChar = ord(succ('A'));
  PredChar = ord(pred('B'));
  LoVal = lo($1234);
  HiVal = hi($1234);

var
  x: integer;

begin
  writeln(MaxSize);
  writeln(MinSize);
  writeln(Range);
  writeln(Doubled);
  writeln(Half);
  writeln(Remainder);
  writeln(NegVal);
  writeln(Flags);
  writeln(Bits);
  writeln(Xored);
  writeln(IsEqual);
  writeln(IsLess);
  writeln(NotTrue);
  writeln(Letter);
  writeln(Computed);
  writeln(Sqr9);
  writeln(Abs42);
  writeln(IsOdd);
  writeln(IsEven);
  writeln(SuccChar);
  writeln(PredChar);
  writeln(LoVal);
  writeln(HiVal);

  { Use constant in expression }
  x := MaxSize + 5;
  writeln(x);
end.
