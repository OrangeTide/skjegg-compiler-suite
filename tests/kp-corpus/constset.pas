program constset;
{ Constant sets. A set constant is pure bits with no runtime handle, so a small
  set (one word) is loaded as a 64-bit immediate and a large set (a set of char
  here, four words) is a read-only word image addressed for membership. Membership
  reads the image word by word and bit by bit. Run under kprun it prints
  1 0 1 0 1 1 0. }
procedure emit(v: integer); external;

type
  TColor = (cRed, cGreen, cBlue, cCyan, cMagenta, cYellow, cWhite, cBlack);
  TCharSet = set of char;

const
  Primary: set of TColor = [cRed, cGreen, cBlue];
  Digits:  TCharSet = ['0'..'9'];
  Hex:     TCharSet = ['0'..'9', 'a'..'f', 'A'..'F'];

begin
  emit(Ord(cGreen in Primary));   { 1 }
  emit(Ord(cWhite in Primary));   { 0 }
  emit(Ord('5' in Digits));       { 1, code 53, first word }
  emit(Ord('a' in Digits));       { 0 }
  emit(Ord('a' in Hex));          { 1, code 97, second word }
  emit(Ord('F' in Hex));          { 1, code 70, second word }
  emit(Ord('g' in Hex));          { 0 }
end.
